#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace nano
{
class ledger;
class network;
class node_config;
class vote_processor;
class votes_cache;
class wallets;

class vote_generator final
{
public:
	vote_generator (nano::node_config const & config_a, nano::ledger &, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::votes_cache & votes_cache_a, nano::network & network_a);
	void add (nano::block_hash const &);
	void stop ();

private:
	void run ();
	void send (nano::unique_lock<std::mutex> &);
	nano::node_config const & config;
	nano::ledger & ledger;
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::votes_cache & votes_cache;
	nano::network & network;
	std::mutex mutex;
	nano::condition_variable condition;
	std::deque<nano::block_hash> hashes;
	nano::network_params network_params;
	bool stopped{ false };
	bool started{ false };
	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (vote_generator & vote_generator, const std::string & name);
};

class vote_generator_session final
{
public:
	vote_generator_session (vote_generator & vote_generator_a);
	void add (nano::block_hash const &);
	void flush ();

private:
	nano::vote_generator & generator;
	std::vector<nano::block_hash> hashes;
};

std::unique_ptr<container_info_component> collect_container_info (vote_generator & vote_generator, const std::string & name);
class cached_votes final
{
public:
	nano::block_hash hash;
	std::vector<std::shared_ptr<nano::vote>> votes;
};
class votes_cache final
{
public:
	votes_cache (nano::wallets & wallets_a);
	void add (std::shared_ptr<nano::vote> const &);
	std::vector<std::shared_ptr<nano::vote>> find (nano::block_hash const &);
	void remove (nano::block_hash const &);

private:
	std::mutex cache_mutex;
	// clang-format off
	class tag_sequence {};
	class tag_hash {};
	boost::multi_index_container<nano::cached_votes,
	boost::multi_index::indexed_by<
		boost::multi_index::sequenced<boost::multi_index::tag<tag_sequence>>,
		boost::multi_index::hashed_unique<boost::multi_index::tag<tag_hash>,
			boost::multi_index::member<nano::cached_votes, nano::block_hash, &nano::cached_votes::hash>>>>
	cache;
	// clang-format on
	nano::network_params network_params;
	nano::wallets & wallets;
	friend std::unique_ptr<container_info_component> collect_container_info (votes_cache & votes_cache, const std::string & name);
};

std::unique_ptr<container_info_component> collect_container_info (votes_cache & votes_cache, const std::string & name);
}
