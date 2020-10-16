#include <nano/lib/alarm.hpp>
#include <nano/lib/threading.hpp>

bool nano::operation::operator> (nano::operation const & other_a) const
{
	return wakeup > other_a.wakeup;
}

nano::alarm::alarm (boost::asio::io_context & io_ctx_a) :
io_ctx (io_ctx_a),
thread ([this]() {
	nano::thread_role::set (nano::thread_role::name::alarm);
	run ();
})
{
}

nano::alarm::~alarm ()
{
	add (std::chrono::steady_clock::now (), nullptr);
	thread.join ();
}

void nano::alarm::run ()
{
	nano::unique_lock<std::mutex> lock (mutex);
	auto done (false);
	while (!done)
	{
		if (!operations.empty ())
		{
			auto & operation (operations.top ());
			if (operation.function)
			{
				if (operation.wakeup <= std::chrono::steady_clock::now ())
				{
					io_ctx.post (operation.function);
					operations.pop ();
				}
				else
				{
					auto wakeup (operation.wakeup);
					condition.wait_until (lock, wakeup);
				}
			}
			else
			{
				done = true;
			}
		}
		else
		{
			condition.wait (lock);
		}
	}
}

void nano::alarm::add (std::chrono::steady_clock::time_point const & wakeup_a, std::function<void()> const & operation)
{
	{
		nano::lock_guard<std::mutex> guard (mutex);
		operations.push (nano::operation ({ wakeup_a, operation }));
	}
	condition.notify_all ();
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (alarm & alarm, const std::string & name)
{
	size_t count;
	{
		nano::lock_guard<std::mutex> guard (alarm.mutex);
		count = alarm.operations.size ();
	}
	auto sizeof_element = sizeof (decltype (alarm.operations)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "operations", count, sizeof_element }));
	return composite;
}
