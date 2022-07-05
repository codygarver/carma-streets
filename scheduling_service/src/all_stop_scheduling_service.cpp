#include "all_stop_scheduling_service.h"

namespace scheduling_service{

	bool all_stop_scheduling_service::initialize(const int sleep_millisecs, const int int_client_request_attempts)
	{
		try
		{
			auto client = std::make_shared<kafka_clients::kafka_client>();
			
			this -> bootstrap_server = streets_service::streets_configuration::get_string_config("bootstrap_server");
			this -> group_id = streets_service::streets_configuration::get_string_config("group_id");
			this -> consumer_topic = streets_service::streets_configuration::get_string_config("consumer_topic");
			this -> producer_topic = streets_service::streets_configuration::get_string_config("producer_topic");


			consumer_worker = client->create_consumer(bootstrap_server, consumer_topic, group_id);
			producer_worker  = client->create_producer(bootstrap_server, producer_topic);

			if(!consumer_worker->init())
			{
				SPDLOG_CRITICAL("kafka consumer initialize error");
				exit(EXIT_FAILURE);
				return false;
			}
			else
			{
				consumer_worker->subscribe();
				if(!consumer_worker->is_running())
				{
					SPDLOG_CRITICAL("consumer_worker is not running");
					exit(EXIT_FAILURE);
					return false;
				}
			}

			// Create logger
			if ( streets_service::streets_configuration::get_boolean_config("enable_schedule_logging") ) {
				configure_csv_logger();
			}
			if(!producer_worker->init())
			{
				SPDLOG_CRITICAL("kafka producer initialize error");
				exit(EXIT_FAILURE);
				return false;
			}
			
			// HTTP request to update intersection information
			auto int_client = std::make_shared<scheduling_service::intersection_client>();

			if (int_client->update_intersection_info(sleep_millisecs, int_client_request_attempts))
			{
				intersection_info_ptr = int_client->get_intersection_info();
			}
			else
			{
				return false;
			}

			vehicle_list_ptr = std::make_shared<streets_vehicles::vehicle_list>();
			config_vehicle_list();

			scheduler_ptr = std::make_shared<streets_vehicle_scheduler::all_stop_vehicle_scheduler>();
			config_scheduler();

			scheduling_worker = std::make_shared<all_stop_scheduling_worker>();

			SPDLOG_INFO("all stop scheduling service initialized successfully!!!");
            return true;
		}
		catch ( const streets_service::streets_configuration_exception &ex ) {
			SPDLOG_ERROR("all stop scheduling service Initialization failure: {0} ", ex.what());
			return false;
		}
	}


	void all_stop_scheduling_service::start()
	{
		std::thread consumer_thread(&all_stop_scheduling_service::consume_msg, this, std::ref(this->consumer_worker), vehicle_list_ptr);
        std::thread scheduling_thread(&all_stop_scheduling_service::schedule_veh, this, std::ref(this->producer_worker), std::ref(this->scheduling_worker), vehicle_list_ptr, scheduler_ptr);
        consumer_thread.join();
        scheduling_thread.join();
	}


	bool all_stop_scheduling_service::config_vehicle_list() const
    {
		if (vehicle_list_ptr)
		{
			vehicle_list_ptr->set_processor(std::make_shared<streets_vehicles::all_stop_status_intent_processor>());
			auto processor = std::dynamic_pointer_cast<streets_vehicles::all_stop_status_intent_processor>(vehicle_list_ptr->get_processor());
			processor->set_stopping_distance(streets_service::streets_configuration::get_double_config("stop_distance"));
			processor->set_stopping_speed(streets_service::streets_configuration::get_double_config("stop_speed"));
			processor->set_timeout(streets_service::streets_configuration::get_int_config("exp_delta"));
			
			SPDLOG_INFO("Vehicle list is configured successfully! ");
			return true;
		}
		else
		{
			return false;
		}
    }


	bool all_stop_scheduling_service::config_scheduler() const
    {
		if (scheduler_ptr && intersection_info_ptr)
		{
			scheduler_ptr->set_intersection_info(intersection_info_ptr);
			scheduler_ptr->set_flexibility_limit(streets_service::streets_configuration::get_int_config("flexibility_limit"));
			
			SPDLOG_INFO("Scheduler is configured successfully! ");
			return true;
		}
		else
		{
			return false;
		}
    }


	void all_stop_scheduling_service::consume_msg(std::shared_ptr<kafka_clients::kafka_consumer_worker> _consumer_worker, std::shared_ptr<streets_vehicles::vehicle_list> _vehicle_list_ptr) const
	{
		
		while (_consumer_worker->is_running()) 
        {
            
            const std::string payload = _consumer_worker->consume(1000);

            if(payload.length() != 0 && _vehicle_list_ptr)
            {                

            	_vehicle_list_ptr->process_update(payload);
    
            }
        }
		_consumer_worker->stop();
	}


	void all_stop_scheduling_service::schedule_veh(std::shared_ptr<kafka_clients::kafka_producer_worker> _producer_worker, std::shared_ptr<all_stop_scheduling_worker> _scheduling_worker, std::shared_ptr<streets_vehicles::vehicle_list> _vehicle_list_ptr, std::shared_ptr<streets_vehicle_scheduler::all_stop_vehicle_scheduler> _scheduler_ptr) const
	{

		if (!_scheduling_worker)
		{
			SPDLOG_CRITICAL("scheduling worker is not initialized");
			exit(EXIT_FAILURE);
		}
		
		u_int64_t last_schedule_timestamp = 0;
		auto scheduling_delta = u_int64_t(streets_service::streets_configuration::get_double_config("scheduling_delta") * 1000);
		int sch_count = 0;
		std::unordered_map<std::string, streets_vehicles::vehicle> veh_map;

		while (true)
		{
			if (_scheduling_worker->start_next_schedule(last_schedule_timestamp, scheduling_delta))
			{
				SPDLOG_DEBUG("schedule number #{0}", sch_count);      
				auto next_schedule_time_epoch = std::chrono::system_clock::now() + std::chrono::milliseconds(scheduling_delta);

				
				veh_map = _vehicle_list_ptr -> get_vehicles();
				streets_vehicle_scheduler::intersection_schedule int_schedule = _scheduling_worker->schedule_vehicles(veh_map, _scheduler_ptr);

				std::string msg_to_send = int_schedule.toJson();

				SPDLOG_DEBUG("schedule plan: {0}", msg_to_send);

				/* produce the scheduling plan to kafka */
				_producer_worker->send(msg_to_send);

				last_schedule_timestamp = int_schedule.timestamp;
				sch_count += 1;

				// sleep until next schedule
				if (std::chrono::system_clock::now() < next_schedule_time_epoch){
					std::this_thread::sleep_until(next_schedule_time_epoch);
				}
			}
		}
		_producer_worker->stop();

	}

 
	void all_stop_scheduling_service::configure_csv_logger() const
	{
		try{
			auto csv_logger = spdlog::daily_logger_mt<spdlog::async_factory>(
				"csv_logger",  // logger name
				streets_service::streets_configuration::get_string_config("schedule_log_path")+
					streets_service::streets_configuration::get_string_config("schedule_log_filename") +".csv",  // log file name and path
				23, // hours to rotate
				59 // minutes to rotate
				);
			// Only log log statement content
			csv_logger->set_pattern("%v");
			csv_logger->set_level(spdlog::level::info);
		}
		catch (const spdlog::spdlog_ex& ex)
		{
			spdlog::error( "Log initialization failed: {0}!",ex.what());
		}
	}


	all_stop_scheduling_service::~all_stop_scheduling_service()
    {
        if (consumer_worker)
        {
            consumer_worker->stop();
        }

        if (producer_worker)
        {
            producer_worker->stop();
        }

    }

}
