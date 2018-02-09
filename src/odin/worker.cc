#include <functional>
#include <string>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <sstream>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "baldr/json.h"
#include "midgard/logging.h"

#include "proto/trippath.pb.h"
#include "odin/worker.h"
#include "odin/util.h"
#include "odin/directionsbuilder.h"
#include "tyr/serializers.h"

using namespace valhalla;
using namespace valhalla::tyr;
using namespace valhalla::midgard;
using namespace valhalla::baldr;

namespace valhalla {
  namespace odin {

    odin_worker_t::odin_worker_t(const boost::property_tree::ptree& config){}

    odin_worker_t::~odin_worker_t(){}

    void odin_worker_t::cleanup(){}

    std::list<TripDirections> odin_worker_t::narrate(const DirectionsOptions& directions_options, std::list<TripPath>& legs) const {
      //get some annotated directions
      std::list<TripDirections> narrated;
      try{
        for(auto& leg : legs) {
          narrated.emplace_back(odin::DirectionsBuilder().Build(directions_options, leg));
          LOG_INFO("maneuver_count::" + std::to_string(narrated.back().maneuver_size()));
        }
      }
      catch(...) {
        throw valhalla_exception_t{202};
      }
      return narrated;
    }

#ifdef HAVE_HTTP
    worker_t::result_t odin_worker_t::work(const std::list<zmq::message_t>& job, void* request_info, const std::function<void ()>& interrupt_function) {
      auto& info = *static_cast<http_request_info_t*>(request_info);
      LOG_INFO("Got Odin Request " + std::to_string(info.id));
      const std::string* jsonp = nullptr;
      try{
        //crack open the original request
        std::string request_str(static_cast<const char*>(job.front().data()), job.front().size());
        rapidjson::Document request;
        auto& allocator = request.GetAllocator();
        request.Parse(request_str.c_str());
        if (request.HasParseError())
          return jsonify_error({200}, info, jsonp);

        //parse it to pbf object
        auto options = from_json(request);
        jsonp = options.has_jsonp() ? &options.jsonp() : nullptr;

        // Set the interrupt function
        service_worker_t::set_interrupt(interrupt_function);

        //parse each leg
        std::list<TripPath> legs;
        for(auto leg = ++job.cbegin(); leg != job.cend(); ++leg) {
          //crack open the path
          legs.emplace_back();
          try {
            legs.back().ParseFromArray(leg->data(), static_cast<int>(leg->size()));
          }
          catch(...) {
            return jsonify_error({201}, info, jsonp);
          }
        }

        //narrate them and serialize them along
        auto narrated = narrate(options, legs);
        //xml
        if(options.format() == DirectionsOptions::gpx)
          return to_response_xml(tyr::serializeDirections(options, legs, narrated), info);
        //json
        return to_response_json(tyr::serializeDirections(options, legs, narrated), info, jsonp);
      }
      catch(const std::exception& e) {
        return jsonify_error({299, std::string(e.what())}, info, jsonp);
      }
    }

    void run_service(const boost::property_tree::ptree& config) {
      //gets requests from odin proxy
      auto upstream_endpoint = config.get<std::string>("odin.service.proxy") + "_out";
      //or returns just location information back to the server
      auto loopback_endpoint = config.get<std::string>("httpd.service.loopback");
      auto interrupt_endpoint = config.get<std::string>("httpd.service.interrupt");

      //listen for requests
      zmq::context_t context;
      prime_server::worker_t worker(context, upstream_endpoint, "ipc:///dev/null", loopback_endpoint, interrupt_endpoint,
        std::bind(&odin_worker_t::work, odin_worker_t(config), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
      worker.work();

      //TODO: should we listen for SIGINT and terminate gracefully/exit(0)?
    }

#endif
  }
}
