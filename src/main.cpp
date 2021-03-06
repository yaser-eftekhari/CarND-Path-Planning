#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2) {
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}

int ClosestWaypoint(double x, double y, vector<double> maps_x, vector<double> maps_y) {

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++) {
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen) {
			closestLen = dist;
			closestWaypoint = i;
		}
	}

	return closestWaypoint;
}

int NextWaypoint(double x, double y, double theta, vector<double> maps_x, vector<double> maps_y) {

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2( (map_y-y),(map_x-x) );

	double angle = abs(theta-heading);

	if(angle > pi()/4) {
		closestWaypoint++;
	}

	return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, vector<double> maps_x, vector<double> maps_y) {
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0) {
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point
	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef) {
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++) {
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};
}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, vector<double> maps_s, vector<double> maps_x, vector<double> maps_y) {
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) )) {
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};
}

void print_array(vector<double> array) {
  for(int i = 0; i < array.size(); i++) {
    printf("%f, ", array[i]);
  }
  printf("\n");
}

const double MPH2MPS = 0.44704;
const double HIGHEST_SPEED = 49.5 * MPH2MPS;
const double SPEED_CHANGE = 0.224 * MPH2MPS;
const double DISTANCE_THRESHOLD_CHANGE_LANE = 7; // if the other car is not 5 m or closer, it is safe to switch lane
const double DISTANCE_THRESHOLD_PATH_PLANNING = 30; // if the other cars are 30 m or closer, take action

// We use only three states. Prepare lane change is discarded as we tend to do more sudden decisions here.
enum planning_state {KL, LCL, LCR};

int lane_index = 1; //left lane is 0, middle lane 1 and right lane 2
double speed_ref = 0; // reference velocity for car to follow (m/sec)
double speed_target = 0;

struct neighboring_car {
  double speed;
  double s;
};

vector<neighboring_car> leading_cars(3); // Nearest cars ahaead for all lanes
vector<neighboring_car> following_cars(3); // Nearest cars behind for all lanes
vector<neighboring_car> cars_in_left_lane; // total leading cars in left lane
vector<neighboring_car> cars_in_middle_lane; // total leading cars in middle lane
vector<neighboring_car> cars_in_right_lane; // total leading cars in right lane

// Initializing variables
void initialize_neighboring_vectors(int lane_index) {
  neighboring_car init_car;
  init_car.s = 0;
  init_car.speed = 0;

  following_cars.erase(following_cars.begin(), following_cars.end());
  following_cars.push_back(init_car);
  following_cars.push_back(init_car);
  following_cars.push_back(init_car);

  init_car.s = 99999;
  init_car.speed = 0;

  leading_cars.erase(leading_cars.begin(), leading_cars.end());
  leading_cars.push_back(init_car);
  leading_cars.push_back(init_car);
  leading_cars.push_back(init_car);

  cars_in_left_lane.erase(cars_in_left_lane.begin(), cars_in_left_lane.end());
  cars_in_middle_lane.erase(cars_in_middle_lane.begin(), cars_in_middle_lane.end());
  cars_in_right_lane.erase(cars_in_right_lane.begin(), cars_in_right_lane.end());
}

int find_lane(double d) {
  if(d > 0 && d < 4)
    return 0;
  if(d > 4 && d < 8)
    return 1;
  return 2;
}

// It makes sense to change lane only if the car in the target lane is further than the one in the current lane
bool does_make_sense_to_change_lane(int from_lane, int to_lane) {
  if(leading_cars[from_lane].s < leading_cars[to_lane].s) {
    return true;
  } else {
    return false;
  }
}

// Decides if it is safe to change lane
bool is_safe_change_lane(int from_lane, int to_lane, double car_s) {
  if(does_make_sense_to_change_lane(from_lane, to_lane)) {
    switch(from_lane) {
      case 0:
        if(((leading_cars[1].s - car_s) > DISTANCE_THRESHOLD_CHANGE_LANE) &&
           ((car_s - following_cars[1].s) > DISTANCE_THRESHOLD_CHANGE_LANE)) {
          if(to_lane == 1) {
            return true;
          } else {
            return is_safe_change_lane(1, 2, car_s);
          }
        }
        return false;
      case 1:
        if(((leading_cars[to_lane].s - car_s) > DISTANCE_THRESHOLD_CHANGE_LANE) &&
           ((car_s - following_cars[to_lane].s) > DISTANCE_THRESHOLD_CHANGE_LANE)) {
          return true;
        }
        return false;
      case 2:
        if(((leading_cars[1].s - car_s) > DISTANCE_THRESHOLD_CHANGE_LANE) &&
            ((car_s - following_cars[1].s) > DISTANCE_THRESHOLD_CHANGE_LANE)) {
          if(to_lane == 1) {
            return true;
          } else {
            return is_safe_change_lane(1, 0, car_s);
          }
        }
        return false;
    }
  }
  return false;
}

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }

  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {
      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);

        string event = j[0].get<string>();

        if (event == "telemetry") {
          // j[1] is the data JSON object

        	// Main car's localization Data
        	double car_x = j[1]["x"];
        	double car_y = j[1]["y"];
        	double car_s = j[1]["s"];
        	double car_d = j[1]["d"];
        	double car_yaw = j[1]["yaw"];
        	double car_speed = j[1]["speed"];

        	// Previous path data given to the Planner
        	auto previous_path_x = j[1]["previous_path_x"];
        	auto previous_path_y = j[1]["previous_path_y"];

        	// Previous path's end s and d values
        	double end_path_s = j[1]["end_path_s"];
        	double end_path_d = j[1]["end_path_d"];

        	// Sensor Fusion Data, a list of all other cars on the same side of the road.
        	auto sensor_fusion = j[1]["sensor_fusion"];

        	json msgJson;

          // Beggining of implementation
          int prev_size = previous_path_x.size();

          if(prev_size > 0) {
            car_s = end_path_s;
          }

          // flag indicating if we have a car in front of us and it is close enough to take action
          bool too_close = false;

          lane_index = find_lane(car_d);

          // Depending on the current lane, which other lanes can we go to
          initialize_neighboring_vectors(lane_index);

          // Check all cars on the road to gather information
          for(int i = 0; i < sensor_fusion.size(); i++) {
            double other_car_vx = sensor_fusion[i][3];
            double other_car_vy = sensor_fusion[i][4];
            double other_car_s = sensor_fusion[i][5];
            double other_car_d = sensor_fusion[i][6];

            // find other cars current lane
            int other_car_lane = find_lane(other_car_d);

            // find the speed of the other car and its s coordinate
            double other_car_speed = distance(0, 0, other_car_vx, other_car_vy);

            // where the other car is going to be after simulator processes the remaining points
            other_car_s += prev_size * 0.02 * other_car_speed;
            neighboring_car this_car;
            this_car.s = other_car_s;
            this_car.speed = other_car_speed;

            // other car is in front
            if(other_car_s > car_s) {
              if(leading_cars[other_car_lane].s > other_car_s) {
                leading_cars[other_car_lane].s = other_car_s;
                leading_cars[other_car_lane].speed = other_car_speed;
              }

              // add the car to the list of the cars in that lane
              switch(other_car_lane) {
                case 0:
                  cars_in_left_lane.push_back(this_car);
                  break;
                case 1:
                  cars_in_middle_lane.push_back(this_car);
                  break;
                case 2:
                  cars_in_right_lane.push_back(this_car);
                  break;
              }
            } else { // other car is following
              if(following_cars[other_car_lane].s < other_car_s) {
                following_cars[other_car_lane].s = other_car_s;
                following_cars[other_car_lane].speed = other_car_speed;
              }
            }

            // if other car is in our lane
            if(other_car_lane == lane_index) {
              // Check to see if the other car is too close to us
              if((other_car_s > car_s) && (other_car_s - car_s < DISTANCE_THRESHOLD_PATH_PLANNING)) {
                too_close = true;
                speed_target = other_car_speed;
              }
            }
          }

          // TODO: consider a cost function for the middle lane to choose the lane which has no car or the car is farther or it is going faster.
          // TODO: a good metric to choose a lane is the number of cars in that lane in front of us
          // TODO: something to consider is if there are cars in front of us which are not still too close to do path planning but there is a lane which has no car in it, we should probably switch lane. So basically another threshold for path planning but more futuristic. Maybe we can switch lane if we find another lane which has no car in it (or less cars)

          // if we detected another car in our lane which is too close, consider changing lane
          planning_state state = KL;
          if(too_close) {
            switch(lane_index) {
              case 0:
                if(is_safe_change_lane(lane_index, 1, car_s)) {
                  lane_index = 1;
                  state = LCR;
                }
                break;
              case 1:
                // consider a lane that has no car first
                if((cars_in_left_lane.size() == 0) &&
                   (is_safe_change_lane(lane_index, 0, car_s))) {
                  lane_index = 0;
                  state = LCL;
                }
                else if((cars_in_right_lane.size() == 0) &&
                        (is_safe_change_lane(lane_index, 2, car_s))) {
                  lane_index = 2;
                  state = LCR;
                }
                // if both lanes have cars in them then choose the lane whose car is farther
                if(state == KL) {
                  if(leading_cars[0].s > leading_cars[2].s) {
                    if(is_safe_change_lane(lane_index, 0, car_s)) {
                      lane_index = 0;
                      state = LCL;
                    }
                  }
                  else if(is_safe_change_lane(lane_index, 2, car_s)) {
                    lane_index = 2;
                    state = LCR;
                  }
                }
                break;
              case 2:
                if(is_safe_change_lane(lane_index, 1, car_s)) {
                  lane_index = 1;
                  state = LCL;
                }
                break;
            }
            if(state == KL) {
              speed_ref = max(speed_ref - SPEED_CHANGE, speed_target); // break with 5 m/s2
            }
          } else {
            // see if any lane is empty to jump to
            switch(lane_index) {
              case 0:
                if(cars_in_left_lane.size() != 0){
                  if((cars_in_middle_lane.size() == 0) &&
                     (is_safe_change_lane(lane_index, 1, car_s))) {
                    lane_index = 1;
                    state = LCR;
                  }
                }
                break;
              case 1:
                if(cars_in_middle_lane.size() != 0) {
                  if((cars_in_left_lane.size() == 0) &&
                     (is_safe_change_lane(lane_index, 0, car_s))) {
                    lane_index = 0;
                    state = LCL;
                  }
                  else if((cars_in_right_lane.size() == 0) &&
                          (is_safe_change_lane(lane_index, 2, car_s))) {
                    lane_index = 2;
                    state = LCR;
                  }
                }
                break;
              case 2:
                if(cars_in_right_lane.size() != 0) {
                  if((cars_in_middle_lane.size() == 0) &&
                     (is_safe_change_lane(lane_index, 1, car_s))) {
                    lane_index = 1;
                    state = LCL;
                  }
                }
                break;
            }
            speed_ref = min(speed_ref + SPEED_CHANGE, HIGHEST_SPEED);
          }

          // vectors to generate path point in
          vector<double> ptsx;
          vector<double> ptsy;

          // reference to where the car is at this instant
          double current_car_x;
          double current_car_y;
          double current_car_yaw;

          // reference to where the car was an instant ago
          double prev_car_x;
          double prev_car_y;

          // generate two points from where the car is
          if(prev_size < 2) {
            current_car_x = car_x;
            current_car_y = car_y;
            current_car_yaw = deg2rad(car_yaw);

            prev_car_x = current_car_x - cos(car_yaw);
            prev_car_y = current_car_y - sin(car_yaw);
          } else {
            current_car_x = previous_path_x[prev_size - 1];
            current_car_y = previous_path_y[prev_size - 1];

            prev_car_x = previous_path_x[prev_size - 2];
            prev_car_y = previous_path_y[prev_size - 2];

            current_car_yaw = atan2(current_car_y - prev_car_y, current_car_x - prev_car_x);
          }
          ptsx.push_back(prev_car_x);
          ptsx.push_back(current_car_x);

          ptsy.push_back(prev_car_y);
          ptsy.push_back(current_car_y);

          // generate three waypoints far apart from where we want to be
          vector<double> next_wp0 = getXY(car_s + 30, 2 + 4 * lane_index, map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp1 = getXY(car_s + 60, 2 + 4 * lane_index, map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp2 = getXY(car_s + 90, 2 + 4 * lane_index, map_waypoints_s, map_waypoints_x, map_waypoints_y);

          ptsx.push_back(next_wp0[0]);
          ptsx.push_back(next_wp1[0]);
          ptsx.push_back(next_wp2[0]);

          ptsy.push_back(next_wp0[1]);
          ptsy.push_back(next_wp1[1]);
          ptsy.push_back(next_wp2[1]);

          // shift the coordinates to be the car coordinates
          for(int i = 0; i < ptsx.size(); i++) {
            // shift x and y to be with reference to the car location
            double shift_x = ptsx[i] - current_car_x;
            double shift_y = ptsy[i] - current_car_y;

            ptsx[i] = (shift_x * cos(current_car_yaw) + shift_y * sin(current_car_yaw));
            ptsy[i] = (shift_y * cos(current_car_yaw) - shift_x * sin(current_car_yaw));
          }

          // create a spline
          tk::spline s;

          // set spline x and y points
          s.set_points(ptsx, ptsy);

          // First move over any remaining points from previous path
          vector<double> next_x_vals = previous_path_x;
        	vector<double> next_y_vals = previous_path_y;

          // Looking 30 m in advance and trying to find desired spacing between the points based on desired speed
          double target_x = 30;
          double target_y = s(target_x);
          double distance2target = distance(0, 0, target_x, target_y);

          // N*0.02*velocity = distance => N = distance2target/(0.02*speed_ref) = 50*distance2target/speed_ref
          double N = 50*distance2target/speed_ref;
          double x_increment = target_x / N;

          // generate remaining waypoints
          for(int i = 0; i < 50 - prev_size; i++) {
            double x_point = (i + 1) * x_increment;
            double y_point = s(x_point);

            double x_ref = x_point;
            double y_ref = y_point;

            x_point = x_ref * cos(current_car_yaw) - y_ref * sin(current_car_yaw);
            y_point = x_ref * sin(current_car_yaw) + y_ref * cos(current_car_yaw);

            x_point += current_car_x;
            y_point += current_car_y;

            next_x_vals.push_back(x_point);
            next_y_vals.push_back(y_point);
          }
          // end of TODO.

        	msgJson["next_x"] = next_x_vals;
        	msgJson["next_y"] = next_y_vals;

        	auto msg = "42[\"control\","+ msgJson.dump()+"]";

        	//this_thread::sleep_for(chrono::milliseconds(1000));
        	ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
