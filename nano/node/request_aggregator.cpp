#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/active_transactions.hpp>
#include <nano/node/common.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/request_aggregator.hpp>
#include <nano/node/transport/udp.hpp>
#include <nano/node/voting.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/ledger.hpp>

nano::request_aggregator::request_aggregator (nano::network_constants const & network_constants_a, nano::node_config const & config_a, nano::stat & stats_a, nano::votes_cache & cache_a, nano::ledger & ledger_a, nano::wallets & wallets_a, nano::active_transactions & active_a) :
max_delay (network_constants_a.is_test_network () ? 50 : 300),
small_delay (network_constants_a.is_test_network () ? 10 : 50),
max_channel_requests (config_a.max_queued_requests),
stats (stats_a),
votes_cache (cache_a),
ledger (ledger_a),
wallets (wallets_a),
active (active_a),
thread ([this]() { run (); })
{
	nano::unique_lock<std::mutex> lock (mutex);
	condition.wait (lock, [& started = started] { return started; });
}

void nano::request_aggregator::add (std::shared_ptr<nano::transport::channel> & channel_a, std::vector<std::pair<nano::block_hash, nano::root>> const & hashes_roots_a)
{
	debug_assert (wallets.reps ().voting > 0);
	bool error = true;
	auto const endpoint (nano::transport::map_endpoint_to_v6 (channel_a->get_endpoint ()));
	nano::unique_lock<std::mutex> lock (mutex);
	// Protecting from ever-increasing memory usage when request are consumed slower than generated
	// Reject request if the oldest request has not yet been processed after its deadline + a modest margin
	if (requests.empty () || (requests.get<tag_deadline> ().begin ()->deadline + 2 * this->max_delay > std::chrono::steady_clock::now ()))
	{
		auto & requests_by_endpoint (requests.get<tag_endpoint> ());
		auto existing (requests_by_endpoint.find (endpoint));
		if (existing == requests_by_endpoint.end ())
		{
			existing = requests_by_endpoint.emplace (channel_a).first;
		}
		requests_by_endpoint.modify (existing, [&hashes_roots_a, &channel_a, &error, this](channel_pool & pool_a) {
			// This extends the lifetime of the channel, which is acceptable up to max_delay
			pool_a.channel = channel_a;
			if (pool_a.hashes_roots.size () + hashes_roots_a.size () <= this->max_channel_requests)
			{
				error = false;
				auto new_deadline (std::min (pool_a.start + this->max_delay, std::chrono::steady_clock::now () + this->small_delay));
				pool_a.deadline = new_deadline;
				pool_a.hashes_roots.insert (pool_a.hashes_roots.begin (), hashes_roots_a.begin (), hashes_roots_a.end ());
			}
		});
		if (requests.size () == 1)
		{
			lock.unlock ();
			condition.notify_all ();
		}
	}
	stats.inc (nano::stat::type::aggregator, !error ? nano::stat::detail::aggregator_accepted : nano::stat::detail::aggregator_dropped);
}

void nano::request_aggregator::run ()
{
	nano::thread_role::set (nano::thread_role::name::request_aggregator);
	nano::unique_lock<std::mutex> lock (mutex);
	started = true;
	lock.unlock ();
	condition.notify_all ();
	lock.lock ();
	while (!stopped)
	{
		if (!requests.empty ())
		{
			auto & requests_by_deadline (requests.get<tag_deadline> ());
			auto front (requests_by_deadline.begin ());
			if (front->deadline < std::chrono::steady_clock::now ())
			{
				// Store the channel and requests for processing after erasing this pool
				decltype (front->channel) channel{};
				decltype (front->hashes_roots) hashes_roots{};
				requests_by_deadline.modify (front, [&channel, &hashes_roots](channel_pool & pool) {
					channel.swap (pool.channel);
					hashes_roots.swap (pool.hashes_roots);
				});
				requests_by_deadline.erase (front);
				lock.unlock ();
				erase_duplicates (hashes_roots);
				auto transaction (ledger.store.tx_begin_read ());
				auto remaining = aggregate (transaction, hashes_roots, channel);
				if (!remaining.empty ())
				{
					// Generate votes for the remaining hashes
					generate (transaction, remaining, channel);
				}
				lock.lock ();
			}
			else
			{
				auto deadline = front->deadline;
				condition.wait_until (lock, deadline, [this, &deadline]() { return this->stopped || deadline < std::chrono::steady_clock::now (); });
			}
		}
		else
		{
			condition.wait_for (lock, small_delay, [this]() { return this->stopped || !this->requests.empty (); });
		}
	}
}

void nano::request_aggregator::stop ()
{
	{
		nano::lock_guard<std::mutex> guard (mutex);
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

std::size_t nano::request_aggregator::size ()
{
	nano::unique_lock<std::mutex> lock (mutex);
	return requests.size ();
}

bool nano::request_aggregator::empty ()
{
	return size () == 0;
}

void nano::request_aggregator::erase_duplicates (std::vector<std::pair<nano::block_hash, nano::root>> & requests_a) const
{
	std::sort (requests_a.begin (), requests_a.end (), [](auto const & pair1, auto const & pair2) {
		return pair1.first < pair2.first;
	});
	requests_a.erase (std::unique (requests_a.begin (), requests_a.end (), [](auto const & pair1, auto const & pair2) {
		return pair1.first == pair2.first;
	}),
	requests_a.end ());
}

std::vector<nano::block_hash> nano::request_aggregator::aggregate (nano::transaction const & transaction_a, std::vector<std::pair<nano::block_hash, nano::root>> const & requests_a, std::shared_ptr<nano::transport::channel> & channel_a) const
{
	size_t cached_hashes = 0;
	std::vector<nano::block_hash> to_generate;
	std::vector<std::shared_ptr<nano::vote>> cached_votes;
	for (auto const & hash_root : requests_a)
	{
		// 1. Votes in cache
		auto find_votes (votes_cache.find (hash_root.first));
		if (!find_votes.empty ())
		{
			++cached_hashes;
			cached_votes.insert (cached_votes.end (), find_votes.begin (), find_votes.end ());
		}
		else
		{
			// 2. Election winner by hash
			auto block = active.winner (hash_root.first);

			// 3. Ledger by hash
			if (block == nullptr)
			{
				block = ledger.store.block_get (transaction_a, hash_root.first);
			}

			// 4. Ledger by root
			if (block == nullptr && !hash_root.second.is_zero ())
			{
				// Search for block root
				auto successor (ledger.store.block_successor (transaction_a, hash_root.second));
				// Search for account root
				if (successor.is_zero ())
				{
					nano::account_info info;
					auto error (ledger.store.account_get (transaction_a, hash_root.second, info));
					if (!error)
					{
						successor = info.open_block;
					}
				}
				if (!successor.is_zero ())
				{
					auto successor_block = ledger.store.block_get (transaction_a, successor);
					debug_assert (successor_block != nullptr);
					// 5. Votes in cache for successor
					auto find_successor_votes (votes_cache.find (successor));
					if (!find_successor_votes.empty ())
					{
						cached_votes.insert (cached_votes.end (), find_successor_votes.begin (), find_successor_votes.end ());
					}
					else
					{
						block = std::move (successor_block);
					}
				}
			}

			if (block)
			{
				// Attempt to vote for this block
				if (ledger.dependents_confirmed (transaction_a, *block))
				{
					to_generate.push_back (block->hash ());
				}
				else
				{
					stats.inc (nano::stat::type::requests, nano::stat::detail::requests_cannot_vote, stat::dir::in);
				}
				// Let the node know about the alternative block
				if (block->hash () != hash_root.first)
				{
					nano::publish publish (block);
					channel_a->send (publish);
				}
			}
			else
			{
				stats.inc (nano::stat::type::requests, nano::stat::detail::requests_unknown, stat::dir::in);
			}
		}
	}
	// Unique votes
	std::sort (cached_votes.begin (), cached_votes.end ());
	cached_votes.erase (std::unique (cached_votes.begin (), cached_votes.end ()), cached_votes.end ());
	for (auto const & vote : cached_votes)
	{
		nano::confirm_ack confirm (vote);
		channel_a->send (confirm);
	}
	stats.add (nano::stat::type::requests, nano::stat::detail::requests_cached_hashes, stat::dir::in, cached_hashes);
	stats.add (nano::stat::type::requests, nano::stat::detail::requests_cached_votes, stat::dir::in, cached_votes.size ());
	return to_generate;
}

void nano::request_aggregator::generate (nano::transaction const & transaction_a, std::vector<nano::block_hash> const & hashes_a, std::shared_ptr<nano::transport::channel> & channel_a) const
{
	size_t generated_l = 0;
	auto i (hashes_a.begin ());
	auto n (hashes_a.end ());
	while (i != n)
	{
		std::vector<nano::block_hash> hashes_l;
		for (; i != n && hashes_l.size () < nano::network::confirm_ack_hashes_max; ++i)
		{
			hashes_l.push_back (*i);
		}
		wallets.foreach_representative ([this, &generated_l, &hashes_l, &channel_a, &transaction_a](nano::public_key const & pub_a, nano::raw_key const & prv_a) {
			auto vote (this->ledger.store.vote_generate (transaction_a, pub_a, prv_a, hashes_l));
			++generated_l;
			nano::confirm_ack confirm (vote);
			channel_a->send (confirm);
			this->votes_cache.add (vote);
		});
	}
	stats.add (nano::stat::type::requests, nano::stat::detail::requests_generated_hashes, stat::dir::in, hashes_a.size ());
	stats.add (nano::stat::type::requests, nano::stat::detail::requests_generated_votes, stat::dir::in, generated_l);
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (nano::request_aggregator & aggregator, const std::string & name)
{
	auto pools_count = aggregator.size ();
	auto sizeof_element = sizeof (decltype (aggregator.requests)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pools", pools_count, sizeof_element }));
	return composite;
}
