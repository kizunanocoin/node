#include <nano/lib/threading.hpp>
#include <nano/lib/worker.hpp>

nano::worker::worker () :
thread ([this]() {
	nano::thread_role::set (nano::thread_role::name::worker);
	this->run ();
})
{
}

void nano::worker::run ()
{
	nano::unique_lock<std::mutex> lk (mutex);
	while (!stopped)
	{
		if (!queue.empty ())
		{
			auto func = queue.front ();
			queue.pop_front ();
			lk.unlock ();
			func ();
			// So that we reduce locking for anything being pushed as that will
			// most likely be on an io-thread
			std::this_thread::yield ();
			lk.lock ();
		}
		else
		{
			cv.wait (lk);
		}
	}
}

nano::worker::~worker ()
{
	stop ();
}

void nano::worker::push_task (std::function<void()> func_a)
{
	{
		nano::lock_guard<std::mutex> guard (mutex);
		if (!stopped)
		{
			queue.emplace_back (func_a);
		}
	}

	cv.notify_one ();
}

void nano::worker::stop ()
{
	{
		nano::unique_lock<std::mutex> lk (mutex);
		stopped = true;
		queue.clear ();
	}
	cv.notify_one ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (nano::worker & worker, const std::string & name)
{
	size_t count;
	{
		nano::lock_guard<std::mutex> guard (worker.mutex);
		count = worker.queue.size ();
	}
	auto sizeof_element = sizeof (decltype (worker.queue)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<nano::container_info_leaf> (nano::container_info{ "queue", count, sizeof_element }));
	return composite;
}
