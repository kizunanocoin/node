#include <nano/core_test/testutil.hpp>
#include <nano/lib/jsonconfig.hpp>
#include <nano/node/election.hpp>
#include <nano/node/testing.hpp>

#include <gtest/gtest.h>

#include <numeric>

using namespace std::chrono_literals;

namespace nano
{
TEST (active_transactions, confirm_active)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node1 = *system.add_node (node_flags);
	nano::genesis genesis;
	auto send (std::make_shared<nano::send_block> (genesis.hash (), nano::public_key (), nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (nano::process_result::progress, node1.process (*send).code);
	nano::node_config node_config2 (nano::get_available_port (), system.logging);
	node_config2.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	nano::node_flags node_flags2;
	// The rep crawler would otherwise request confirmations in order to find representatives
	node_flags2.disable_rep_crawler = true;
	auto & node2 = *system.add_node (node_config2, node_flags2);
	system.deadline_set (5s);
	// Let node2 know about the block
	while (node2.active.empty ())
	{
		node1.network.flood_block (send, nano::buffer_drop_policy::no_limiter_drop);
		ASSERT_NO_ERROR (system.poll ());
	}
	// Save election to check request count afterwards
	auto election = node2.active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election);
	// Add key to node1
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	// Add representative to disabled rep crawler
	auto peers (node2.network.random_set (1));
	ASSERT_FALSE (peers.empty ());
	{
		nano::lock_guard<std::mutex> guard (node2.rep_crawler.probable_reps_mutex);
		node2.rep_crawler.probable_reps.emplace (nano::test_genesis_key.pub, nano::genesis_amount, *peers.begin ());
	}
	while (node2.ledger.cache.cemented_count < 2 || !node2.active.empty ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	// At least one confirmation request
	ASSERT_GT (election->confirmation_request_count, 0u);
	// Blocks were cleared (except for not_an_account)
	ASSERT_EQ (1, election->blocks.size ());
}
}

namespace nano
{
TEST (active_transactions, confirm_frontier)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node1 = *system.add_node (node_flags);
	nano::genesis genesis;
	auto send (std::make_shared<nano::send_block> (genesis.hash (), nano::public_key (), nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (nano::process_result::progress, node1.process (*send).code);
	nano::node_flags node_flags2;
	// The rep crawler would otherwise request confirmations in order to find representatives
	node_flags2.disable_rep_crawler = true;
	auto & node2 = *system.add_node (node_flags2);
	ASSERT_EQ (nano::process_result::progress, node2.process (*send).code);
	system.deadline_set (5s);
	while (node2.active.empty ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	// Save election to check request count afterwards
	auto election = node2.active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election);
	// Add key to node1
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	// Add representative to disabled rep crawler
	auto peers (node2.network.random_set (1));
	ASSERT_FALSE (peers.empty ());
	{
		nano::lock_guard<std::mutex> guard (node2.rep_crawler.probable_reps_mutex);
		node2.rep_crawler.probable_reps.emplace (nano::test_genesis_key.pub, nano::genesis_amount, *peers.begin ());
	}
	system.deadline_set (5s);
	while (node2.ledger.cache.cemented_count < 2 || !node2.active.empty ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_GT (election->confirmation_request_count, 0u);
}
}

TEST (active_transactions, keep_local)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.enable_voting = false;
	node_config.active_elections_size = 2; //bound to 2, wont drop wallet created transactions, but good to test dropping remote
	// Disable frontier confirmation to allow the test to finish before
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	auto & wallet (*system.wallet (0));
	nano::genesis genesis;
	//key 1/2 will be managed by the wallet
	nano::keypair key1, key2, key3, key4, key5, key6;
	wallet.insert_adhoc (nano::test_genesis_key.prv);
	auto send1 (wallet.send_action (nano::test_genesis_key.pub, key1.pub, node.config.receive_minimum.number ()));
	auto send2 (wallet.send_action (nano::test_genesis_key.pub, key2.pub, node.config.receive_minimum.number ()));
	auto send3 (wallet.send_action (nano::test_genesis_key.pub, key3.pub, node.config.receive_minimum.number ()));
	auto send4 (wallet.send_action (nano::test_genesis_key.pub, key4.pub, node.config.receive_minimum.number ()));
	auto send5 (wallet.send_action (nano::test_genesis_key.pub, key5.pub, node.config.receive_minimum.number ()));
	auto send6 (wallet.send_action (nano::test_genesis_key.pub, key6.pub, node.config.receive_minimum.number ()));
	system.deadline_set (5s);
	// should not drop wallet created transactions
	while (node.active.size () != 6)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (0, node.active.recently_dropped.size ());
	while (!node.active.empty ())
	{
		nano::lock_guard<std::mutex> active_guard (node.active.mutex);
		if (!node.active.roots.empty ())
		{
			node.active.roots.begin ()->election->confirm_once ();
		}
	}
	auto open1 (std::make_shared<nano::state_block> (key1.pub, 0, key1.pub, node.config.receive_minimum.number (), send1->hash (), key1.prv, key1.pub, *system.work.generate (key1.pub)));
	auto open2 (std::make_shared<nano::state_block> (key2.pub, 0, key2.pub, node.config.receive_minimum.number (), send2->hash (), key2.prv, key2.pub, *system.work.generate (key2.pub)));
	auto open3 (std::make_shared<nano::state_block> (key3.pub, 0, key3.pub, node.config.receive_minimum.number (), send3->hash (), key3.prv, key3.pub, *system.work.generate (key3.pub)));
	node.process_active (open1);
	node.process_active (open2);
	node.process_active (open3);
	node.block_processor.flush ();
	system.deadline_set (5s);
	// bound elections, should drop after one loop
	while (node.active.size () != node_config.active_elections_size)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node.active.recently_dropped.size ());
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_drop));
}

TEST (active_transactions, inactive_votes_cache)
{
	nano::system system (1);
	auto & node = *system.nodes[0];
	nano::block_hash latest (node.latest (nano::test_genesis_key.pub));
	nano::keypair key;
	auto send (std::make_shared<nano::send_block> (latest, key.pub, nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest)));
	auto vote (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, std::vector<nano::block_hash> (1, send->hash ())));
	node.vote_processor.vote (vote, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	system.deadline_set (5s);
	while (node.active.inactive_votes_cache_size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node.process_active (send);
	node.block_processor.flush ();
	system.deadline_set (5s);
	while (!node.ledger.block_confirmed (node.store.tx_begin_read (), send->hash ()))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_cached));
}

TEST (active_transactions, inactive_votes_cache_fork)
{
	nano::system system (1);
	auto & node = *system.nodes[0];
	nano::block_hash latest (node.latest (nano::test_genesis_key.pub));
	nano::keypair key;
	auto send1 (std::make_shared<nano::send_block> (latest, key.pub, nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest)));
	auto send2 (std::make_shared<nano::send_block> (latest, key.pub, nano::genesis_amount - 200, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest)));
	auto vote (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, std::vector<nano::block_hash> (1, send1->hash ())));
	node.vote_processor.vote (vote, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	auto channel1 (node.network.udp_channels.create (node.network.endpoint ()));
	system.deadline_set (5s);
	while (node.active.inactive_votes_cache_size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node.network.process_message (nano::publish (send2), channel1);
	node.block_processor.flush ();
	ASSERT_NE (nullptr, node.block (send2->hash ()));
	node.network.process_message (nano::publish (send1), channel1);
	node.block_processor.flush ();
	bool confirmed (false);
	system.deadline_set (5s);
	while (!confirmed)
	{
		auto transaction (node.store.tx_begin_read ());
		confirmed = node.block (send1->hash ()) != nullptr && node.ledger.block_confirmed (transaction, send1->hash ()) && node.active.empty ();
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_cached));
}

TEST (active_transactions, inactive_votes_cache_existing_vote)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::block_hash latest (node.latest (nano::test_genesis_key.pub));
	nano::keypair key;
	auto send (std::make_shared<nano::send_block> (latest, key.pub, nano::genesis_amount - 100 * nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest)));
	auto open (std::make_shared<nano::state_block> (key.pub, 0, key.pub, 100 * nano::Gxrb_ratio, send->hash (), key.prv, key.pub, *system.work.generate (key.pub))); // Increase key weight
	node.process_active (send);
	node.block_processor.add (open);
	node.block_processor.flush ();
	system.deadline_set (5s);
	while (node.active.size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	std::shared_ptr<nano::election> election;
	{
		nano::lock_guard<std::mutex> active_guard (node.active.mutex);
		auto it (node.active.roots.begin ());
		ASSERT_NE (node.active.roots.end (), it);
		election = it->election;
	}
	ASSERT_GT (node.weight (key.pub), node.minimum_principal_weight ());
	// Insert vote
	auto vote1 (std::make_shared<nano::vote> (key.pub, key.prv, 1, std::vector<nano::block_hash> (1, send->hash ())));
	node.vote_processor.vote (vote1, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	system.deadline_set (5s);
	bool done (false);
	while (!done)
	{
		nano::unique_lock<std::mutex> active_lock (node.active.mutex);
		done = (election->last_votes.size () == 2);
		active_lock.unlock ();
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_new));
	nano::lock_guard<std::mutex> active_guard (node.active.mutex);
	auto last_vote1 (election->last_votes[key.pub]);
	ASSERT_EQ (send->hash (), last_vote1.hash);
	ASSERT_EQ (1, last_vote1.sequence);
	// Attempt to change vote with inactive_votes_cache
	node.active.add_inactive_votes_cache (send->hash (), key.pub);
	ASSERT_EQ (1, node.active.find_inactive_votes_cache (send->hash ()).voters.size ());
	election->insert_inactive_votes_cache (send->hash ());
	// Check that election data is not changed
	ASSERT_EQ (2, election->last_votes.size ());
	auto last_vote2 (election->last_votes[key.pub]);
	ASSERT_EQ (last_vote1.hash, last_vote2.hash);
	ASSERT_EQ (last_vote1.sequence, last_vote2.sequence);
	ASSERT_EQ (last_vote1.time, last_vote2.time);
	ASSERT_EQ (0, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_cached));
}

TEST (active_transactions, inactive_votes_cache_multiple_votes)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::block_hash latest (node.latest (nano::test_genesis_key.pub));
	nano::keypair key1;
	auto send1 (std::make_shared<nano::send_block> (latest, key1.pub, nano::genesis_amount - 100 * nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest)));
	auto send2 (std::make_shared<nano::send_block> (send1->hash (), key1.pub, 100 * nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1->hash ()))); // Decrease genesis weight to prevent election confirmation
	auto open (std::make_shared<nano::state_block> (key1.pub, 0, key1.pub, 100 * nano::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *system.work.generate (key1.pub))); // Increase key1 weight
	node.block_processor.add (send1);
	node.block_processor.add (send2);
	node.block_processor.add (open);
	node.block_processor.flush ();
	// Process votes
	auto vote1 (std::make_shared<nano::vote> (key1.pub, key1.prv, 0, std::vector<nano::block_hash> (1, send1->hash ())));
	node.vote_processor.vote (vote1, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	auto vote2 (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, std::vector<nano::block_hash> (1, send1->hash ())));
	node.vote_processor.vote (vote2, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	system.deadline_set (5s);
	while (true)
	{
		{
			nano::lock_guard<std::mutex> active_guard (node.active.mutex);
			if (node.active.find_inactive_votes_cache (send1->hash ()).voters.size () == 2)
			{
				break;
			}
		}
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node.active.inactive_votes_cache_size ());
	// Start election
	node.active.insert (send1);
	{
		nano::lock_guard<std::mutex> active_guard (node.active.mutex);
		auto it (node.active.roots.begin ());
		ASSERT_NE (node.active.roots.end (), it);
		ASSERT_EQ (3, it->election->last_votes.size ()); // 2 votes and 1 default not_an_acount
	}
	ASSERT_EQ (2, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_cached));
}

TEST (active_transactions, inactive_votes_cache_election_start)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::block_hash latest (node.latest (nano::test_genesis_key.pub));
	nano::keypair key1, key2, key3, key4, key5;
	nano::send_block_builder send_block_builder;
	nano::state_block_builder state_block_builder;
	std::shared_ptr<nano::block> send1 = send_block_builder.make_block ()
	                                     .previous (latest)
	                                     .destination (key1.pub)
	                                     .balance (nano::genesis_amount - 2000 * nano::Gxrb_ratio)
	                                     .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                     .work (*system.work.generate (latest))
	                                     .build ();
	std::shared_ptr<nano::block> send2 = send_block_builder.make_block ()
	                                     .previous (send1->hash ())
	                                     .destination (key2.pub)
	                                     .balance (nano::genesis_amount - 4000 * nano::Gxrb_ratio)
	                                     .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                     .work (*system.work.generate (send1->hash ()))
	                                     .build ();
	std::shared_ptr<nano::block> send3 = send_block_builder.make_block ()
	                                     .previous (send2->hash ())
	                                     .destination (key3.pub)
	                                     .balance (nano::genesis_amount - 6000 * nano::Gxrb_ratio)
	                                     .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                     .work (*system.work.generate (send2->hash ()))
	                                     .build ();
	std::shared_ptr<nano::block> send4 = send_block_builder.make_block ()
	                                     .previous (send3->hash ())
	                                     .destination (key4.pub)
	                                     .balance (nano::genesis_amount - 8000 * nano::Gxrb_ratio)
	                                     .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                     .work (*system.work.generate (send3->hash ()))
	                                     .build ();
	std::shared_ptr<nano::block> send5 = send_block_builder.make_block ()
	                                     .previous (send4->hash ())
	                                     .destination (key5.pub)
	                                     .balance (nano::genesis_amount - 10000 * nano::Gxrb_ratio)
	                                     .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                     .work (*system.work.generate (send4->hash ()))
	                                     .build ();
	std::shared_ptr<nano::block> open1 = state_block_builder.make_block ()
	                                     .account (key1.pub)
	                                     .previous (0)
	                                     .representative (key1.pub)
	                                     .balance (2000 * nano::Gxrb_ratio)
	                                     .link (send1->hash ())
	                                     .sign (key1.prv, key1.pub)
	                                     .work (*system.work.generate (key1.pub))
	                                     .build ();
	std::shared_ptr<nano::block> open2 = state_block_builder.make_block ()
	                                     .account (key2.pub)
	                                     .previous (0)
	                                     .representative (key2.pub)
	                                     .balance (2000 * nano::Gxrb_ratio)
	                                     .link (send2->hash ())
	                                     .sign (key2.prv, key2.pub)
	                                     .work (*system.work.generate (key2.pub))
	                                     .build ();
	std::shared_ptr<nano::block> open3 = state_block_builder.make_block ()
	                                     .account (key3.pub)
	                                     .previous (0)
	                                     .representative (key3.pub)
	                                     .balance (2000 * nano::Gxrb_ratio)
	                                     .link (send3->hash ())
	                                     .sign (key3.prv, key3.pub)
	                                     .work (*system.work.generate (key3.pub))
	                                     .build ();
	std::shared_ptr<nano::block> open4 = state_block_builder.make_block ()
	                                     .account (key4.pub)
	                                     .previous (0)
	                                     .representative (key4.pub)
	                                     .balance (2000 * nano::Gxrb_ratio)
	                                     .link (send4->hash ())
	                                     .sign (key4.prv, key4.pub)
	                                     .work (*system.work.generate (key4.pub))
	                                     .build ();
	std::shared_ptr<nano::block> open5 = state_block_builder.make_block ()
	                                     .account (key5.pub)
	                                     .previous (0)
	                                     .representative (key5.pub)
	                                     .balance (2000 * nano::Gxrb_ratio)
	                                     .link (send5->hash ())
	                                     .sign (key5.prv, key5.pub)
	                                     .work (*system.work.generate (key5.pub))
	                                     .build ();
	node.block_processor.add (send1);
	node.block_processor.add (send2);
	node.block_processor.add (send3);
	node.block_processor.add (send4);
	node.block_processor.add (send5);
	node.block_processor.add (open1);
	node.block_processor.add (open2);
	node.block_processor.add (open3);
	node.block_processor.add (open4);
	node.block_processor.add (open5);
	node.block_processor.flush ();
	ASSERT_TIMELY (5s, 11 == node.ledger.cache.block_count);
	ASSERT_TRUE (node.active.empty ());
	ASSERT_EQ (1, node.ledger.cache.cemented_count);
	// These blocks will be processed later
	std::shared_ptr<nano::block> send6 = send_block_builder.make_block ()
	                                     .previous (send5->hash ())
	                                     .destination (nano::keypair ().pub)
	                                     .balance (send5->balance ().number () - 1)
	                                     .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                     .work (*system.work.generate (send5->hash ()))
	                                     .build ();
	std::shared_ptr<nano::block> send7 = send_block_builder.make_block ()
	                                     .previous (send6->hash ())
	                                     .destination (nano::keypair ().pub)
	                                     .balance (send6->balance ().number () - 1)
	                                     .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                     .work (*system.work.generate (send6->hash ()))
	                                     .build ();
	// Inactive votes
	std::vector<nano::block_hash> hashes{ open1->hash (), open2->hash (), open3->hash (), open4->hash (), open5->hash (), send7->hash () };
	auto vote1 (std::make_shared<nano::vote> (key1.pub, key1.prv, 0, hashes));
	node.vote_processor.vote (vote1, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	auto vote2 (std::make_shared<nano::vote> (key2.pub, key2.prv, 0, hashes));
	node.vote_processor.vote (vote2, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	auto vote3 (std::make_shared<nano::vote> (key3.pub, key3.prv, 0, hashes));
	node.vote_processor.vote (vote3, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	auto vote4 (std::make_shared<nano::vote> (key4.pub, key4.prv, 0, hashes));
	node.vote_processor.vote (vote4, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	ASSERT_TIMELY (5s, node.active.inactive_votes_cache_size () == 6);
	ASSERT_TRUE (node.active.empty ());
	ASSERT_EQ (1, node.ledger.cache.cemented_count);
	// 5 votes are required to start election
	auto vote5 (std::make_shared<nano::vote> (key5.pub, key5.prv, 0, hashes));
	node.vote_processor.vote (vote5, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	ASSERT_TIMELY (5s, 5 == node.active.size ());
	// Confirm elections with weight quorum
	auto vote0 (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, hashes));
	node.vote_processor.vote (vote0, std::make_shared<nano::transport::channel_udp> (node.network.udp_channels, node.network.endpoint (), node.network_params.protocol.protocol_version));
	ASSERT_TIMELY (5s, node.active.empty ());
	ASSERT_TIMELY (5s, 11 == node.ledger.cache.cemented_count);
	// A late block arrival also checks the inactive votes cache
	ASSERT_TRUE (node.active.empty ());
	auto send7_cache (node.active.find_inactive_votes_cache (send7->hash ()));
	ASSERT_EQ (6, send7_cache.voters.size ());
	ASSERT_TRUE (send7_cache.status.bootstrap_started);
	ASSERT_TRUE (send7_cache.status.confirmed);
	ASSERT_TRUE (send7_cache.status.election_started); // already marked even though the block does not exist
	node.process_active (send6);
	node.block_processor.flush ();
	// An election is started for send6 but does not confirm
	ASSERT_TIMELY (5s, 1 == node.active.size ());
	node.vote_processor.flush ();
	ASSERT_FALSE (node.block_confirmed_or_being_confirmed (node.store.tx_begin_read (), send6->hash ()));
	// send7 cannot be voted on but an election should be started from inactive votes
	ASSERT_FALSE (node.ledger.dependents_confirmed (node.store.tx_begin_read (), *send7));
	node.process_active (send7);
	node.block_processor.flush ();
	ASSERT_TIMELY (5s, 13 == node.ledger.cache.cemented_count);
}

TEST (active_transactions, update_difficulty)
{
	nano::system system (2);
	auto & node1 = *system.nodes[0];
	auto & node2 = *system.nodes[1];
	nano::genesis genesis;
	nano::keypair key1;
	// Generate blocks & start elections
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - 100, key1.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	auto difficulty1 (send1->difficulty ());
	auto multiplier1 (nano::normalized_multiplier (nano::difficulty::to_multiplier (difficulty1, nano::work_threshold (send1->work_version (), nano::block_details (nano::epoch::epoch_0, true, false, false))), node1.network_params.network.publish_thresholds.epoch_1));
	auto send2 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, send1->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 200, key1.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1->hash ())));
	auto difficulty2 (send2->difficulty ());
	auto multiplier2 (nano::normalized_multiplier (nano::difficulty::to_multiplier (difficulty2, nano::work_threshold (send2->work_version (), nano::block_details (nano::epoch::epoch_0, true, false, false))), node1.network_params.network.publish_thresholds.epoch_1));
	node1.process_active (send1);
	node1.process_active (send2);
	node1.block_processor.flush ();
	ASSERT_NO_ERROR (system.poll_until_true (10s, [&node1, &node2] { return node1.active.size () == 2 && node2.active.size () == 2; }));
	// Update work with higher difficulty
	auto work1 = node1.work_generate_blocking (send1->root (), difficulty1 + 1);
	auto work2 = node1.work_generate_blocking (send2->root (), difficulty2 + 1);

	std::error_code ec;
	nano::state_block_builder builder;
	send1 = std::shared_ptr<nano::state_block> (builder.from (*send1).work (*work1).build (ec));
	nano::state_block_builder builder1;
	send2 = std::shared_ptr<nano::state_block> (builder1.from (*send2).work (*work2).build (ec));
	ASSERT_FALSE (ec);

	node1.process_active (send1);
	node1.process_active (send2);
	node1.block_processor.flush ();
	// Share the updated blocks
	node1.network.flood_block (send1);
	node1.network.flood_block (send2);

	system.deadline_set (10s);
	bool done (false);
	while (!done)
	{
		{
			// node1
			nano::lock_guard<std::mutex> guard1 (node1.active.mutex);
			auto const existing1 (node1.active.roots.find (send1->qualified_root ()));
			ASSERT_NE (existing1, node1.active.roots.end ());
			auto const existing2 (node1.active.roots.find (send2->qualified_root ()));
			ASSERT_NE (existing2, node1.active.roots.end ());
			// node2
			nano::lock_guard<std::mutex> guard2 (node2.active.mutex);
			auto const existing3 (node2.active.roots.find (send1->qualified_root ()));
			ASSERT_NE (existing3, node2.active.roots.end ());
			auto const existing4 (node2.active.roots.find (send2->qualified_root ()));
			ASSERT_NE (existing4, node2.active.roots.end ());
			auto updated1 = existing1->multiplier > multiplier1;
			auto updated2 = existing2->multiplier > multiplier2;
			auto propagated1 = existing3->multiplier > multiplier1;
			auto propagated2 = existing4->multiplier > multiplier2;
			done = updated1 && updated2 && propagated1 && propagated2;
		}
		ASSERT_NO_ERROR (system.poll ());
	}
}

namespace nano
{
TEST (active_transactions, vote_replays)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.enable_voting = false;
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::genesis genesis;
	nano::keypair key;
	std::error_code ec;
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_NE (nullptr, send1);
	auto open1 (std::make_shared<nano::state_block> (key.pub, 0, key.pub, nano::Gxrb_ratio, send1->hash (), key.prv, key.pub, *system.work.generate (key.pub)));
	ASSERT_NE (nullptr, open1);
	node.process_active (send1);
	node.process_active (open1);
	nano::blocks_confirm (node, { send1, open1 });
	ASSERT_EQ (2, node.active.size ());
	// First vote is not a replay and confirms the election, second vote should be a replay since the election has confirmed but not yet removed
	auto vote_send1 (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, send1));
	ASSERT_EQ (nano::vote_code::vote, node.active.vote (vote_send1));
	ASSERT_EQ (2, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote_send1));
	// Wait until the election is removed, at which point the vote is still a replay since it's been recently confirmed
	ASSERT_TIMELY (3s, node.active.size () == 1);
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote_send1));
	// Open new account
	auto vote_open1 (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, open1));
	ASSERT_EQ (nano::vote_code::vote, node.active.vote (vote_open1));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote_open1));
	ASSERT_TIMELY (3s, node.active.empty ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote_open1));
	ASSERT_EQ (nano::Gxrb_ratio, node.ledger.weight (key.pub));

	auto send2 (std::make_shared<nano::state_block> (key.pub, open1->hash (), key.pub, nano::Gxrb_ratio - 1, key.pub, key.prv, key.pub, *system.work.generate (open1->hash ())));
	ASSERT_NE (nullptr, send2);
	node.process_active (send2);
	nano::blocks_confirm (node, { send2 });
	ASSERT_EQ (1, node.active.size ());
	auto vote1_send2 (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, send2));
	auto vote2_send2 (std::make_shared<nano::vote> (key.pub, key.prv, 0, send2));
	ASSERT_EQ (nano::vote_code::vote, node.active.vote (vote2_send2));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote2_send2));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (nano::vote_code::vote, node.active.vote (vote1_send2));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote1_send2));
	ASSERT_TIMELY (3s, node.active.empty ());
	ASSERT_EQ (0, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote1_send2));
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote2_send2));

	// Removing blocks as recently confirmed makes every vote indeterminate
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		node.active.recently_confirmed.clear ();
	}
	ASSERT_EQ (nano::vote_code::indeterminate, node.active.vote (vote_send1));
	ASSERT_EQ (nano::vote_code::indeterminate, node.active.vote (vote_open1));
	ASSERT_EQ (nano::vote_code::indeterminate, node.active.vote (vote1_send2));
	ASSERT_EQ (nano::vote_code::indeterminate, node.active.vote (vote2_send2));
}
}

namespace nano
{
// Tests that blocks are correctly cleared from the duplicate filter for unconfirmed elections
TEST (active_transactions, dropped_cleanup)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node (*system.add_node (node_config));

	nano::genesis genesis;
	auto block = genesis.open;
	block->sideband_set (nano::block_sideband (nano::genesis_account, 0, nano::genesis_amount, 1, nano::seconds_since_epoch (), nano::epoch::epoch_0, false, false, false));

	// Add to network filter to ensure proper cleanup after the election is dropped
	std::vector<uint8_t> block_bytes;
	{
		nano::vectorstream stream (block_bytes);
		block->serialize (stream);
	}
	ASSERT_FALSE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));
	ASSERT_TRUE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));

	auto election (node.active.insert (block).election);
	ASSERT_NE (nullptr, election);

	// Not yet removed
	ASSERT_TRUE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));

	// Now simulate dropping the election, which performs a cleanup in the background using the node worker
	ASSERT_FALSE (election->confirmed ());
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		election->cleanup ();
	}

	// Push a worker task to ensure the cleanup is already performed
	std::atomic<bool> flag{ false };
	node.worker.push_task ([&flag]() {
		flag = true;
	});
	system.deadline_set (5s);
	while (!flag)
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	// The filter must have been cleared
	ASSERT_FALSE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));
}
}

namespace nano
{
// Blocks that won an election must always be seen as confirming or cemented
TEST (active_transactions, confirmation_consistency)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	for (unsigned i = 0; i < 10; ++i)
	{
		auto block (system.wallet (0)->send_action (nano::test_genesis_key.pub, nano::public_key (), node.config.receive_minimum.number ()));
		ASSERT_NE (nullptr, block);
		system.deadline_set (5s);
		while (!node.ledger.block_confirmed (node.store.tx_begin_read (), block->hash ()))
		{
			ASSERT_FALSE (node.active.insert (block).inserted);
			ASSERT_NO_ERROR (system.poll (5ms));
		}
		ASSERT_NO_ERROR (system.poll_until_true (1s, [&node, &block, i] {
			nano::lock_guard<std::mutex> guard (node.active.mutex);
			EXPECT_EQ (i + 1, node.active.recently_confirmed.size ());
			EXPECT_EQ (block->qualified_root (), node.active.recently_confirmed.back ().first);
			return i + 1 == node.active.recently_cemented.size (); // done after a callback
		}));
	}
}
}

TEST (active_transactions, insertion_prioritization)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	// 10% of elections (1) are prioritized
	node_config.active_elections_size = 10;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_config, node_flags);
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, nano::genesis_hash, nano::test_genesis_key.pub, nano::genesis_amount - 10 * nano::xrb_ratio, nano::public_key (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (nano::genesis_hash)));
	auto send2 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, send1->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 20 * nano::xrb_ratio, nano::public_key (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1->hash ())));
	auto send3 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, send2->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 30 * nano::xrb_ratio, nano::public_key (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send2->hash ())));
	auto send4 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, send3->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 40 * nano::xrb_ratio, nano::public_key (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send3->hash ())));
	auto send5 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, send4->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 50 * nano::xrb_ratio, nano::public_key (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send4->hash ())));
	auto send6 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, send5->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 60 * nano::xrb_ratio, nano::public_key (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send5->hash ())));
	auto send7 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, send6->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 70 * nano::xrb_ratio, nano::public_key (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send6->hash ())));

	// Sort by difficulty, descending
	std::vector<std::shared_ptr<nano::block>> blocks{ send1, send2, send3, send4, send5, send6, send7 };
	for (auto const & block : blocks)
	{
		ASSERT_EQ (nano::process_result::progress, node.process (*block).code);
	}
	std::sort (blocks.begin (), blocks.end (), [](auto const & blockl, auto const & blockr) { return blockl->difficulty () > blockr->difficulty (); });

	auto update_active_multiplier = [&node] {
		nano::unique_lock<std::mutex> lock (node.active.mutex);
		node.active.update_active_multiplier (lock);
	};

	ASSERT_TRUE (node.active.insert (blocks[2]).election->prioritized ());
	update_active_multiplier ();
	ASSERT_FALSE (node.active.insert (blocks[3]).election->prioritized ());
	update_active_multiplier ();
	ASSERT_TRUE (node.active.insert (blocks[1]).election->prioritized ());
	update_active_multiplier ();
	ASSERT_FALSE (node.active.insert (blocks[4]).election->prioritized ());
	update_active_multiplier ();
	ASSERT_TRUE (node.active.insert (blocks[0]).election->prioritized ());
	update_active_multiplier ();
	ASSERT_FALSE (node.active.insert (blocks[5]).election->prioritized ());
	update_active_multiplier ();
	ASSERT_FALSE (node.active.insert (blocks[6]).election->prioritized ());

	ASSERT_EQ (4, node.stats.count (nano::stat::type::election, nano::stat::detail::election_non_priority));
	ASSERT_EQ (3, node.stats.count (nano::stat::type::election, nano::stat::detail::election_priority));
}

TEST (active_multiplier, less_than_one)
{
	nano::system system (1);
	auto & node (*system.nodes[0]);
	nano::unique_lock<std::mutex> lock (node.active.mutex);
	auto base_active_difficulty = node.network_params.network.publish_thresholds.epoch_1;
	auto base_active_multiplier = 1.0;
	auto min_active_difficulty = node.network_params.network.publish_thresholds.entry;
	auto min_multiplier = nano::difficulty::to_multiplier (min_active_difficulty, base_active_difficulty);
	ASSERT_EQ (node.active.trended_active_multiplier, base_active_multiplier);
	for (int i = 0; i < node.active.multipliers_cb.size () - 1; ++i)
	{
		node.active.multipliers_cb.push_front (min_multiplier);
	}
	auto sum (std::accumulate (node.active.multipliers_cb.begin (), node.active.multipliers_cb.end (), double(0)));
	auto multiplier = sum / node.active.multipliers_cb.size ();
	node.active.multipliers_cb.push_front (min_multiplier);
	node.active.update_active_multiplier (lock);
	ASSERT_EQ (node.active.trended_active_multiplier, multiplier);
}

TEST (active_multiplier, normalization)
{
	nano::system system (1);
	auto & node (*system.nodes[0]);
	// Check normalization for epoch 1
	double multiplier1 (1.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier1, node.network_params.network.publish_thresholds.epoch_1), nano::difficulty::from_multiplier (1.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier1 (nano::normalized_multiplier (multiplier1, node.network_params.network.publish_thresholds.epoch_1));
	ASSERT_NEAR (1.0, norm_multiplier1, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier1, node.network_params.network.publish_thresholds.epoch_1), multiplier1, 1e-10);
	double multiplier2 (5.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier2, node.network_params.network.publish_thresholds.epoch_1), nano::difficulty::from_multiplier (1.5, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier2 (nano::normalized_multiplier (multiplier2, node.network_params.network.publish_thresholds.epoch_1));
	ASSERT_NEAR (1.5, norm_multiplier2, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier2, node.network_params.network.publish_thresholds.epoch_1), multiplier2, 1e-10);
	double multiplier3 (9.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier3, node.network_params.network.publish_thresholds.epoch_1), nano::difficulty::from_multiplier (2.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier3 (nano::normalized_multiplier (multiplier3, node.network_params.network.publish_thresholds.epoch_1));
	ASSERT_NEAR (2.0, norm_multiplier3, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier3, node.network_params.network.publish_thresholds.epoch_1), multiplier3, 1e-10);
	double multiplier4 (17.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier4, node.network_params.network.publish_thresholds.epoch_1), nano::difficulty::from_multiplier (3.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier4 (nano::normalized_multiplier (multiplier4, node.network_params.network.publish_thresholds.epoch_1));
	ASSERT_NEAR (3.0, norm_multiplier4, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier4, node.network_params.network.publish_thresholds.epoch_1), multiplier4, 1e-10);
	double multiplier5 (25.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier5, node.network_params.network.publish_thresholds.epoch_1), nano::difficulty::from_multiplier (4.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier5 (nano::normalized_multiplier (multiplier5, node.network_params.network.publish_thresholds.epoch_1));
	ASSERT_NEAR (4.0, norm_multiplier5, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier5, node.network_params.network.publish_thresholds.epoch_1), multiplier5, 1e-10);
	double multiplier6 (57.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier6, node.network_params.network.publish_thresholds.epoch_1), nano::difficulty::from_multiplier (8.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier6 (nano::normalized_multiplier (multiplier6, node.network_params.network.publish_thresholds.epoch_1));
	ASSERT_NEAR (8.0, norm_multiplier6, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier6, node.network_params.network.publish_thresholds.epoch_1), multiplier6, 1e-10);
	// Check normalization for epoch 2 receive
	double multiplier10 (1.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier10, node.network_params.network.publish_thresholds.epoch_2_receive), nano::difficulty::from_multiplier (1.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier10 (nano::normalized_multiplier (multiplier10, node.network_params.network.publish_thresholds.epoch_2_receive));
	ASSERT_NEAR (1.0, norm_multiplier10, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier10, node.network_params.network.publish_thresholds.epoch_2_receive), multiplier10, 1e-10);
	double multiplier11 (33.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier11, node.network_params.network.publish_thresholds.epoch_2_receive), nano::difficulty::from_multiplier (1.5, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier11 (nano::normalized_multiplier (multiplier11, node.network_params.network.publish_thresholds.epoch_2_receive));
	ASSERT_NEAR (1.5, norm_multiplier11, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier11, node.network_params.network.publish_thresholds.epoch_2_receive), multiplier11, 1e-10);
	double multiplier12 (65.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier12, node.network_params.network.publish_thresholds.epoch_2_receive), nano::difficulty::from_multiplier (2.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier12 (nano::normalized_multiplier (multiplier12, node.network_params.network.publish_thresholds.epoch_2_receive));
	ASSERT_NEAR (2.0, norm_multiplier12, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier12, node.network_params.network.publish_thresholds.epoch_2_receive), multiplier12, 1e-10);
	double multiplier13 (129.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier13, node.network_params.network.publish_thresholds.epoch_2_receive), nano::difficulty::from_multiplier (3.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier13 (nano::normalized_multiplier (multiplier13, node.network_params.network.publish_thresholds.epoch_2_receive));
	ASSERT_NEAR (3.0, norm_multiplier13, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier13, node.network_params.network.publish_thresholds.epoch_2_receive), multiplier13, 1e-10);
	double multiplier14 (193.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier14, node.network_params.network.publish_thresholds.epoch_2_receive), nano::difficulty::from_multiplier (4.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier14 (nano::normalized_multiplier (multiplier14, node.network_params.network.publish_thresholds.epoch_2_receive));
	ASSERT_NEAR (4.0, norm_multiplier14, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier14, node.network_params.network.publish_thresholds.epoch_2_receive), multiplier14, 1e-10);
	double multiplier15 (961.0);
	ASSERT_LT (nano::difficulty::from_multiplier (multiplier15, node.network_params.network.publish_thresholds.epoch_2_receive), nano::difficulty::from_multiplier (16.0, node.network_params.network.publish_thresholds.epoch_2));
	auto norm_multiplier15 (nano::normalized_multiplier (multiplier15, node.network_params.network.publish_thresholds.epoch_2_receive));
	ASSERT_NEAR (16.0, norm_multiplier15, 1e-10);
	ASSERT_NEAR (nano::denormalized_multiplier (norm_multiplier15, node.network_params.network.publish_thresholds.epoch_2_receive), multiplier15, 1e-10);
}

namespace nano
{
TEST (active_transactions, vote_generator_session)
{
	nano::system system (1);
	auto node (system.nodes[0]);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::vote_generator_session generator_session (node->active.generator);
	boost::thread thread ([node, &generator_session]() {
		nano::thread_role::set (nano::thread_role::name::request_loop);
		for (unsigned i = 0; i < 100; ++i)
		{
			generator_session.add (nano::genesis_hash);
		}
		ASSERT_EQ (0, node->stats.count (nano::stat::type::vote, nano::stat::detail::vote_indeterminate));
		generator_session.flush ();
	});
	thread.join ();
	system.deadline_set (5s);
	while (node->stats.count (nano::stat::type::vote, nano::stat::detail::vote_indeterminate) < (100 / nano::network::confirm_ack_hashes_max))
	{
		ASSERT_NO_ERROR (system.poll (5ms));
	}
}
}

TEST (active_transactions, election_difficulty_update_old)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_flags);
	nano::genesis genesis;
	nano::keypair key;
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - 10 * nano::xrb_ratio, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	auto send1_copy (std::make_shared<nano::state_block> (*send1));
	node.process_active (send1);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	auto multiplier = node.active.roots.begin ()->multiplier;
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		ASSERT_EQ (node.active.normalized_multiplier (*send1), multiplier);
	}
	// Should not update with a lower difficulty
	send1_copy->block_work_set (0);
	ASSERT_EQ (nano::process_result::old, node.process (*send1_copy).code);
	ASSERT_FALSE (send1_copy->has_sideband ());
	node.process_active (send1);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (node.active.roots.begin ()->multiplier, multiplier);
	// Update work, even without a sideband it should find the block in the election and update the election multiplier
	ASSERT_TRUE (node.work_generate_blocking (*send1_copy, send1->difficulty () + 1).is_initialized ());
	node.process_active (send1_copy);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_GT (node.active.roots.begin ()->multiplier, multiplier);

	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_difficulty_update));
}

TEST (active_transactions, election_difficulty_update_fork)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_flags);

	ASSERT_NE (nullptr, system.upgrade_genesis_epoch (node, nano::epoch::epoch_1));
	auto epoch2 = system.upgrade_genesis_epoch (node, nano::epoch::epoch_2);
	ASSERT_NE (nullptr, epoch2);
	nano::keypair key;
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, epoch2->hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (epoch2->hash ())));
	auto open1 (std::make_shared<nano::state_block> (key.pub, 0, key.pub, nano::Gxrb_ratio, send1->hash (), key.prv, key.pub, *system.work.generate (key.pub)));
	auto send2 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, send1->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 2 * nano::Gxrb_ratio, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (nano::process_result::progress, node.process (*send1).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*open1).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);
	// Confirm blocks so far to allow starting elections for upcoming blocks
	for (auto block : { open1, send2 })
	{
		node.block_confirm (block);
		{
			auto election = node.active.election (block->qualified_root ());
			ASSERT_NE (nullptr, election);
			nano::lock_guard<std::mutex> guard (node.active.mutex);
			election->confirm_once ();
		}
		ASSERT_TIMELY (2s, node.block_confirmed (block->hash ()));
		node.active.erase (*block);
	}

	// Verify an election with multiple blocks is correctly updated on arrival of another block
	// Each subsequent block has difficulty at least higher than the previous one
	auto fork_change (std::make_shared<nano::state_block> (key.pub, open1->hash (), nano::test_genesis_key.pub, nano::Gxrb_ratio, 0, key.prv, key.pub, *system.work.generate (open1->hash ())));
	auto fork_send (std::make_shared<nano::state_block> (key.pub, open1->hash (), key.pub, 0, key.pub, key.prv, key.pub, *system.work.generate (open1->hash (), fork_change->difficulty ())));
	auto fork_receive (std::make_shared<nano::state_block> (key.pub, open1->hash (), key.pub, 2 * nano::Gxrb_ratio, send2->hash (), key.prv, key.pub, *system.work.generate (open1->hash (), fork_send->difficulty ())));
	ASSERT_GT (fork_send->difficulty (), fork_change->difficulty ());
	ASSERT_GT (fork_receive->difficulty (), fork_send->difficulty ());

	node.process_active (fork_change);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	auto multiplier_change = node.active.roots.begin ()->multiplier;
	node.process_active (fork_send);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_block_conflict));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_difficulty_update));
	auto multiplier_send = node.active.roots.begin ()->multiplier;
	node.process_active (fork_receive);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (2, node.stats.count (nano::stat::type::election, nano::stat::detail::election_block_conflict));
	ASSERT_EQ (2, node.stats.count (nano::stat::type::election, nano::stat::detail::election_difficulty_update));
	auto multiplier_receive = node.active.roots.begin ()->multiplier;

	ASSERT_GT (multiplier_send, multiplier_change);
	ASSERT_GT (multiplier_receive, multiplier_send);

	EXPECT_FALSE (fork_receive->has_sideband ());
	auto threshold = nano::work_threshold (fork_receive->work_version (), nano::block_details (nano::epoch::epoch_2, false, true, false));
	auto denormalized = nano::denormalized_multiplier (multiplier_receive, threshold);
	ASSERT_NEAR (nano::difficulty::to_multiplier (fork_receive->difficulty (), threshold), denormalized, 1e-10);

	// Ensure a fork with updated difficulty will also update the election difficulty
	fork_receive->block_work_set (*system.work.generate (fork_receive->root (), fork_receive->difficulty () + 1));
	node.process_active (fork_receive);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (2, node.stats.count (nano::stat::type::election, nano::stat::detail::election_block_conflict));
	ASSERT_EQ (3, node.stats.count (nano::stat::type::election, nano::stat::detail::election_difficulty_update));
	auto multiplier_receive_updated = node.active.roots.begin ()->multiplier;
	ASSERT_GT (multiplier_receive_updated, multiplier_receive);
}

TEST (active_transactions, confirm_new)
{
	nano::system system (1);
	auto & node1 = *system.nodes[0];
	nano::genesis genesis;
	auto send (std::make_shared<nano::send_block> (genesis.hash (), nano::public_key (), nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	node1.process_active (send);
	node1.block_processor.flush ();
	ASSERT_EQ (1, node1.active.size ());
	auto & node2 = *system.add_node ();
	// Add key to node2
	system.wallet (1)->insert_adhoc (nano::test_genesis_key.prv);
	system.deadline_set (5s);
	// Let node2 know about the block
	while (node2.block (send->hash ()) == nullptr)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	// Wait confirmation
	while (node1.ledger.cache.cemented_count < 2 || node2.ledger.cache.cemented_count < 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (active_transactions, restart_dropped)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::genesis genesis;
	auto send (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::xrb_ratio, nano::test_genesis_key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	// Process only in ledger and simulate dropping the election
	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);
	node.active.recently_dropped.add (send->qualified_root ());
	// Generate higher difficulty work
	ASSERT_TRUE (node.work_generate_blocking (*send, send->difficulty () + 1).is_initialized ());
	// Process the same block with updated work
	ASSERT_EQ (0, node.active.size ());
	node.process_active (send);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_restart));
	auto ledger_block (node.store.block_get (node.store.tx_begin_read (), send->hash ()));
	ASSERT_NE (nullptr, ledger_block);
	// Exact same block, including work value must have been re-written
	ASSERT_EQ (*send, *ledger_block);
	// Removed from the dropped elections cache
	ASSERT_EQ (std::chrono::steady_clock::time_point{}, node.active.recently_dropped.find (send->qualified_root ()));
	// Drop election
	node.active.erase (*send);
	ASSERT_EQ (0, node.active.size ());
	// Try to restart election with the same difficulty
	node.process_active (send);
	node.block_processor.flush ();
	ASSERT_EQ (0, node.active.size ());
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_restart));
	// Verify the block was not updated in the ledger
	ASSERT_EQ (*node.store.block_get (node.store.tx_begin_read (), send->hash ()), *send);
	// Generate even higher difficulty work
	ASSERT_TRUE (node.work_generate_blocking (*send, send->difficulty () + 1).is_initialized ());
	// Add voting
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	// Process the same block with updated work
	ASSERT_EQ (0, node.active.size ());
	node.process_active (send);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (1, node.ledger.cache.cemented_count);
	ASSERT_EQ (2, node.stats.count (nano::stat::type::election, nano::stat::detail::election_restart));
	// Wait for the election to complete
	ASSERT_TIMELY (5s, node.ledger.cache.cemented_count == 2);
}

// Ensures votes are tallied on election::publish even if no vote is inserted through inactive_votes_cache
TEST (active_transactions, conflicting_block_vote_existing_election)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_flags);
	nano::genesis genesis;
	nano::keypair key;
	auto send (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - 100, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	auto fork (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - 200, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	auto vote_fork (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, fork));

	ASSERT_EQ (nano::process_result::progress, node.process_local (send).code);
	ASSERT_EQ (1, node.active.size ());

	// Vote for conflicting block, but the block does not yet exist in the ledger
	node.active.vote (vote_fork);

	// Block now gets processed
	ASSERT_EQ (nano::process_result::fork, node.process_local (fork).code);

	// Election must be confirmed
	auto election (node.active.election (fork->qualified_root ()));
	ASSERT_NE (nullptr, election);
	ASSERT_TRUE (election->confirmed ());
}

TEST (active_transactions, activate_account_chain)
{
	nano::system system;
	nano::node_flags flags;
	nano::node_config config (nano::get_available_port (), system.logging);
	config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (config, flags);

	nano::keypair key;
	nano::state_block_builder builder;
	auto send = builder.make_block ()
	            .account (nano::test_genesis_key.pub)
	            .previous (nano::genesis_hash)
	            .representative (nano::test_genesis_key.pub)
	            .link (nano::test_genesis_key.pub)
	            .balance (nano::genesis_amount - 1)
	            .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	            .work (*system.work.generate (nano::genesis_hash))
	            .build ();
	auto send2 = builder.make_block ()
	             .account (nano::test_genesis_key.pub)
	             .previous (send->hash ())
	             .representative (nano::test_genesis_key.pub)
	             .link (key.pub)
	             .balance (nano::genesis_amount - 2)
	             .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	             .work (*system.work.generate (send->hash ()))
	             .build ();
	auto send3 = builder.make_block ()
	             .account (nano::test_genesis_key.pub)
	             .previous (send2->hash ())
	             .representative (nano::test_genesis_key.pub)
	             .link (key.pub)
	             .balance (nano::genesis_amount - 3)
	             .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	             .work (*system.work.generate (send2->hash ()))
	             .build ();
	auto open = builder.make_block ()
	            .account (key.pub)
	            .previous (0)
	            .representative (key.pub)
	            .link (send2->hash ())
	            .balance (1)
	            .sign (key.prv, key.pub)
	            .work (*system.work.generate (key.pub))
	            .build ();
	auto receive = builder.make_block ()
	               .account (key.pub)
	               .previous (open->hash ())
	               .representative (key.pub)
	               .link (send3->hash ())
	               .balance (2)
	               .sign (key.prv, key.pub)
	               .work (*system.work.generate (open->hash ()))
	               .build ();
	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*send3).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*open).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*receive).code);

	auto result = node.active.activate (nano::test_genesis_key.pub);
	ASSERT_TRUE (result.inserted);
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (1, result.election->blocks.count (send->hash ()));
	auto result2 = node.active.activate (nano::test_genesis_key.pub);
	ASSERT_FALSE (result2.inserted);
	ASSERT_EQ (result2.election, result.election);
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		result.election->confirm_once ();
	}
	ASSERT_TIMELY (3s, node.block_confirmed (send->hash ()));
	// On cementing, the next election is started
	ASSERT_TIMELY (3s, node.active.active (send2->qualified_root ()));
	auto result3 = node.active.activate (nano::test_genesis_key.pub);
	ASSERT_FALSE (result3.inserted);
	ASSERT_NE (nullptr, result3.election);
	ASSERT_EQ (1, result3.election->blocks.count (send2->hash ()));
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		result3.election->confirm_once ();
	}
	ASSERT_TIMELY (3s, node.block_confirmed (send2->hash ()));
	// On cementing, the next election is started
	ASSERT_TIMELY (3s, node.active.active (open->qualified_root ()));
	ASSERT_TIMELY (3s, node.active.active (send3->qualified_root ()));
	auto result4 = node.active.activate (nano::test_genesis_key.pub);
	ASSERT_FALSE (result4.inserted);
	ASSERT_NE (nullptr, result4.election);
	ASSERT_EQ (1, result4.election->blocks.count (send3->hash ()));
	auto result5 = node.active.activate (key.pub);
	ASSERT_FALSE (result5.inserted);
	ASSERT_NE (nullptr, result5.election);
	ASSERT_EQ (1, result5.election->blocks.count (open->hash ()));
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		result5.election->confirm_once ();
	}
	ASSERT_TIMELY (3s, node.block_confirmed (open->hash ()));
	// Until send3 is also confirmed, the receive block should not activate
	std::this_thread::sleep_for (200ms);
	auto result6 = node.active.activate (key.pub);
	ASSERT_FALSE (result6.inserted);
	ASSERT_EQ (nullptr, result6.election);
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		result4.election->confirm_once ();
	}
	ASSERT_TIMELY (3s, node.block_confirmed (send3->hash ()));
	ASSERT_TIMELY (3s, node.active.active (receive->qualified_root ()));
}

TEST (active_transactions, activate_inactive)
{
	nano::system system;
	nano::node_flags flags;
	nano::node_config config (nano::get_available_port (), system.logging);
	config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (config, flags);

	nano::keypair key;
	nano::state_block_builder builder;
	std::shared_ptr<nano::block> send = builder.make_block ()
	                                    .account (nano::test_genesis_key.pub)
	                                    .previous (nano::genesis_hash)
	                                    .representative (nano::test_genesis_key.pub)
	                                    .link (key.pub)
	                                    .balance (nano::genesis_amount - 1)
	                                    .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                    .work (*system.work.generate (nano::genesis_hash))
	                                    .build ();
	std::shared_ptr<nano::block> send2 = builder.make_block ()
	                                     .account (nano::test_genesis_key.pub)
	                                     .previous (send->hash ())
	                                     .representative (nano::test_genesis_key.pub)
	                                     .link (nano::keypair ().pub)
	                                     .balance (nano::genesis_amount - 2)
	                                     .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                     .work (*system.work.generate (send->hash ()))
	                                     .build ();
	std::shared_ptr<nano::block> open = builder.make_block ()
	                                    .account (key.pub)
	                                    .previous (0)
	                                    .representative (key.pub)
	                                    .link (send->hash ())
	                                    .balance (1)
	                                    .sign (key.prv, key.pub)
	                                    .work (*system.work.generate (key.pub))
	                                    .build ();

	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*open).code);

	node.block_confirm (send2);
	{
		auto election = node.active.election (send2->qualified_root ());
		ASSERT_NE (nullptr, election);
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		election->confirm_once ();
	}

	ASSERT_TIMELY (3s, !node.confirmation_height_processor.is_processing_block (send2->hash ()));
	ASSERT_TRUE (node.block_confirmed (send2->hash ()));
	ASSERT_TRUE (node.block_confirmed (send->hash ()));

	ASSERT_EQ (1, node.stats.count (nano::stat::type::confirmation_observer, nano::stat::detail::inactive_conf_height, nano::stat::dir::out));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::confirmation_observer, nano::stat::detail::active_quorum, nano::stat::dir::out));
	ASSERT_EQ (0, node.stats.count (nano::stat::type::confirmation_observer, nano::stat::detail::active_conf_height, nano::stat::dir::out));

	// The first block was not active so no activation takes place
	ASSERT_FALSE (node.active.active (open->qualified_root ()) || node.block_confirmed_or_being_confirmed (node.store.tx_begin_read (), open->hash ()));
}

TEST (active_transactions, difficulty_update_observer)
{
	nano::system system (1);
	auto & node (*system.nodes[0]);
	std::atomic<bool> update_received (false);
	node.observers.difficulty.add ([& mutex = node.active.mutex, &update_received](uint64_t difficulty_a) {
		nano::unique_lock<std::mutex> lock (mutex, std::defer_lock);
		EXPECT_TRUE (lock.try_lock ());
		update_received = true;
	});
	ASSERT_TIMELY (3s, update_received);
}

namespace nano
{
TEST (active_transactions, pessimistic_elections)
{
	nano::system system;
	nano::node_flags flags;
	nano::node_config config (nano::get_available_port (), system.logging);
	config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (config, flags);

	nano::keypair key;
	nano::state_block_builder builder;
	std::shared_ptr<nano::block> send = builder.make_block ()
	                                    .account (nano::test_genesis_key.pub)
	                                    .previous (nano::genesis_hash)
	                                    .representative (nano::test_genesis_key.pub)
	                                    .link (nano::test_genesis_key.pub)
	                                    .balance (nano::genesis_amount - 1)
	                                    .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                    .work (*system.work.generate (nano::genesis_hash))
	                                    .build ();

	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);

	auto send2 = builder.make_block ()
	             .account (nano::test_genesis_key.pub)
	             .previous (send->hash ())
	             .representative (nano::test_genesis_key.pub)
	             .link (key.pub)
	             .balance (nano::genesis_amount - 2)
	             .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	             .work (*system.work.generate (send->hash ()))
	             .build ();

	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);

	std::shared_ptr<nano::block> open = builder.make_block ()
	                                    .account (key.pub)
	                                    .previous (0)
	                                    .representative (key.pub)
	                                    .link (send2->hash ())
	                                    .balance (1)
	                                    .sign (key.prv, key.pub)
	                                    .work (*system.work.generate (key.pub))
	                                    .build ();

	ASSERT_EQ (nano::process_result::progress, node.process (*open).code);

	// This should only cement the first block in genesis account
	uint64_t election_count = 0;
	// Make dummy election with winner.
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		nano::election election1 (
		node, send, [](auto const & block) {}, false, nano::election_behavior::normal);
		nano::election election2 (
		node, open, [](auto const & block) {}, false, nano::election_behavior::normal);
		node.active.add_expired_optimistic_election (election1);
		node.active.add_expired_optimistic_election (election2);
	}
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
	auto election_started_it = node.active.expired_optimistic_election_infos.get<nano::active_transactions::tag_election_started> ().begin ();
	ASSERT_EQ (election_started_it->account, nano::genesis_account);
	ASSERT_EQ (election_started_it->election_started, true);
	ASSERT_EQ ((++election_started_it)->election_started, false);

	// No new elections should get started yet
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
	ASSERT_EQ (node.active.expired_optimistic_election_infos_size, node.active.expired_optimistic_election_infos.size ());

	{
		ASSERT_EQ (1, node.active.size ());
		auto election = node.active.election (send->qualified_root ());
		ASSERT_NE (nullptr, election);
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		election->confirm_once ();
	}

	ASSERT_TIMELY (3s, node.block_confirmed (send->hash ()) && !node.confirmation_height_processor.is_processing_block (send->hash ()));

	nano::confirmation_height_info genesis_confirmation_height_info;
	nano::confirmation_height_info key1_confirmation_height_info;
	{
		auto transaction = node.store.tx_begin_read ();
		node.store.confirmation_height_get (transaction, nano::genesis_account, genesis_confirmation_height_info);
		ASSERT_EQ (2, genesis_confirmation_height_info.height);
		node.store.confirmation_height_get (transaction, key.pub, key1_confirmation_height_info);
		ASSERT_EQ (0, key1_confirmation_height_info.height);
	}

	// Activation of cemented frontier successor should get started after the first pessimistic block is confirmed
	ASSERT_TIMELY (10s, node.active.active (send->qualified_root ()));

	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());

	// Confirm it
	{
		auto election = node.active.election (send2->qualified_root ());
		ASSERT_NE (nullptr, election);
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		election->confirm_once ();
	}

	ASSERT_TIMELY (3s, node.block_confirmed (send2->hash ()));

	{
		auto transaction = node.store.tx_begin_read ();
		node.store.confirmation_height_get (transaction, nano::genesis_account, genesis_confirmation_height_info);
		ASSERT_EQ (3, genesis_confirmation_height_info.height);
		node.store.confirmation_height_get (transaction, key.pub, key1_confirmation_height_info);
		ASSERT_EQ (0, key1_confirmation_height_info.height);
	}

	// Wait until activation of destination account is done.
	ASSERT_TIMELY (10s, node.active.active (send2->qualified_root ()));

	// Election count should not increase, but the elections should be marked as started for that account afterwards
	ASSERT_EQ (election_started_it->election_started, false);
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);

	{
		auto election = node.active.election (open->qualified_root ());
		ASSERT_NE (nullptr, election);
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		election->confirm_once ();
	}

	ASSERT_TIMELY (3s, node.block_confirmed (open->hash ()));

	{
		auto transaction = node.store.tx_begin_read ();
		node.store.confirmation_height_get (transaction, nano::genesis_account, genesis_confirmation_height_info);
		ASSERT_EQ (3, genesis_confirmation_height_info.height);
		node.store.confirmation_height_get (transaction, key.pub, key1_confirmation_height_info);
		ASSERT_EQ (1, key1_confirmation_height_info.height);
	}

	// Sanity check that calling it again on a fully cemented chain has no adverse effects.
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
}
}
