#include "transport/udp.hpp"

#include <nano/lib/threading.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/node/voting.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/ledger.hpp>

#include <boost/variant/get.hpp>

#include <chrono>

nano::vote_generator::vote_generator (nano::node_config const & config_a, nano::ledger & ledger_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::votes_cache & votes_cache_a, nano::network & network_a) :
config (config_a),
ledger (ledger_a),
wallets (wallets_a),
vote_processor (vote_processor_a),
votes_cache (votes_cache_a),
network (network_a),
thread ([this]() { run (); })
{
	nano::unique_lock<std::mutex> lock (mutex);
	condition.wait (lock, [& started = started] { return started; });
}

void nano::vote_generator::add (nano::block_hash const & hash_a)
{
	auto transaction (ledger.store.tx_begin_read ());
	nano::unique_lock<std::mutex> lock (mutex);
	auto block (ledger.store.block_get (transaction, hash_a));
	if (block != nullptr && ledger.dependents_confirmed (transaction, *block))
	{
		hashes.push_back (hash_a);
		if (hashes.size () >= nano::network::confirm_ack_hashes_max)
		{
			lock.unlock ();
			condition.notify_all ();
		}
	}
}

void nano::vote_generator::stop ()
{
	nano::unique_lock<std::mutex> lock (mutex);
	stopped = true;

	lock.unlock ();
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::vote_generator::send (nano::unique_lock<std::mutex> & lock_a)
{
	std::vector<nano::block_hash> hashes_l;
	hashes_l.reserve (nano::network::confirm_ack_hashes_max);
	while (!hashes.empty () && hashes_l.size () < nano::network::confirm_ack_hashes_max)
	{
		hashes_l.push_back (hashes.front ());
		hashes.pop_front ();
	}
	lock_a.unlock ();
	{
		auto transaction (ledger.store.tx_begin_read ());
		wallets.foreach_representative ([this, &hashes_l, &transaction](nano::public_key const & pub_a, nano::raw_key const & prv_a) {
			auto vote (this->ledger.store.vote_generate (transaction, pub_a, prv_a, hashes_l));
			this->votes_cache.add (vote);
			this->network.flood_vote_pr (vote);
			this->network.flood_vote (vote, 2.0f);
			this->vote_processor.vote (vote, std::make_shared<nano::transport::channel_udp> (this->network.udp_channels, this->network.endpoint (), this->network_params.protocol.protocol_version));
		});
	}
	lock_a.lock ();
}

void nano::vote_generator::run ()
{
	nano::thread_role::set (nano::thread_role::name::voting);
	nano::unique_lock<std::mutex> lock (mutex);
	started = true;
	lock.unlock ();
	condition.notify_all ();
	lock.lock ();
	while (!stopped)
	{
		if (hashes.size () >= nano::network::confirm_ack_hashes_max)
		{
			send (lock);
		}
		else
		{
			condition.wait_for (lock, config.vote_generator_delay, [this]() { return this->hashes.size () >= nano::network::confirm_ack_hashes_max; });
			if (hashes.size () >= config.vote_generator_threshold && hashes.size () < nano::network::confirm_ack_hashes_max)
			{
				condition.wait_for (lock, config.vote_generator_delay, [this]() { return this->hashes.size () >= nano::network::confirm_ack_hashes_max; });
			}
			if (!hashes.empty ())
			{
				send (lock);
			}
		}
	}
}

nano::vote_generator_session::vote_generator_session (nano::vote_generator & vote_generator_a) :
generator (vote_generator_a)
{
}

void nano::vote_generator_session::add (nano::block_hash const & hash_a)
{
	debug_assert (nano::thread_role::get () == nano::thread_role::name::request_loop);
	hashes.push_back (hash_a);
}

void nano::vote_generator_session::flush ()
{
	debug_assert (nano::thread_role::get () == nano::thread_role::name::request_loop);
	for (auto const & i : hashes)
	{
		generator.add (i);
	}
}

nano::votes_cache::votes_cache (nano::wallets & wallets_a) :
wallets (wallets_a)
{
}

void nano::votes_cache::add (std::shared_ptr<nano::vote> const & vote_a)
{
	auto voting (wallets.reps ().voting);
	if (voting == 0)
	{
		return;
	}
	nano::lock_guard<std::mutex> lock (cache_mutex);
	auto const max_cache_size (network_params.voting.max_cache / std::max (voting, static_cast<decltype (voting)> (1)));
	for (auto & block : vote_a->blocks)
	{
		auto hash (boost::get<nano::block_hash> (block));
		auto existing (cache.get<tag_hash> ().find (hash));
		if (existing == cache.get<tag_hash> ().end ())
		{
			// Clean old votes
			if (cache.size () >= max_cache_size)
			{
				cache.get<tag_sequence> ().pop_front ();
			}
			// Insert new votes (new hash)
			auto inserted (cache.get<tag_sequence> ().emplace_back (nano::cached_votes{ hash, std::vector<std::shared_ptr<nano::vote>> (1, vote_a) }));
			(void)inserted;
			debug_assert (inserted.second);
		}
		else
		{
			// Insert new votes (old hash)
			cache.get<tag_hash> ().modify (existing, [vote_a](nano::cached_votes & cache_a) {
				// Replace old vote for same representative & hash
				bool replaced (false);
				for (auto i (cache_a.votes.begin ()), n (cache_a.votes.end ()); i != n && !replaced; ++i)
				{
					if ((*i)->account == vote_a->account)
					{
						*i = vote_a;
						replaced = true;
					}
				}
				// Insert new vote
				if (!replaced)
				{
					cache_a.votes.push_back (vote_a);
				}
			});
		}
	}
}

std::vector<std::shared_ptr<nano::vote>> nano::votes_cache::find (nano::block_hash const & hash_a)
{
	std::vector<std::shared_ptr<nano::vote>> result;
	nano::lock_guard<std::mutex> lock (cache_mutex);
	auto existing (cache.get<tag_hash> ().find (hash_a));
	if (existing != cache.get<tag_hash> ().end ())
	{
		result = existing->votes;
	}
	return result;
}

void nano::votes_cache::remove (nano::block_hash const & hash_a)
{
	nano::lock_guard<std::mutex> lock (cache_mutex);
	cache.get<tag_hash> ().erase (hash_a);
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (vote_generator & vote_generator, const std::string & name)
{
	size_t hashes_count = 0;

	{
		nano::lock_guard<std::mutex> guard (vote_generator.mutex);
		hashes_count = vote_generator.hashes.size ();
	}
	auto sizeof_element = sizeof (decltype (vote_generator.hashes)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "state_blocks", hashes_count, sizeof_element }));
	return composite;
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (votes_cache & votes_cache, const std::string & name)
{
	size_t cache_count;

	{
		nano::lock_guard<std::mutex> guard (votes_cache.cache_mutex);
		cache_count = votes_cache.cache.size ();
	}
	auto sizeof_element = sizeof (decltype (votes_cache.cache)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	/* This does not currently loop over each element inside the cache to get the sizes of the votes inside cached_votes */
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "cache", cache_count, sizeof_element }));
	return composite;
}
