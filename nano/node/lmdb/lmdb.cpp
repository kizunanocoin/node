#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/common.hpp>
#include <nano/node/lmdb/lmdb.hpp>
#include <nano/node/lmdb/lmdb_iterator.hpp>
#include <nano/node/lmdb/wallet_value.hpp>
#include <nano/secure/buffer.hpp>
#include <nano/secure/versioning.hpp>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/polymorphic_cast.hpp>

#include <queue>

namespace nano
{
template <>
void * mdb_val::data () const
{
	return value.mv_data;
}

template <>
size_t mdb_val::size () const
{
	return value.mv_size;
}

template <>
mdb_val::db_val (size_t size_a, void * data_a) :
value ({ size_a, data_a })
{
}

template <>
void mdb_val::convert_buffer_to_value ()
{
	value = { buffer->size (), const_cast<uint8_t *> (buffer->data ()) };
}
}

nano::mdb_store::mdb_store (nano::logger_mt & logger_a, boost::filesystem::path const & path_a, nano::txn_tracking_config const & txn_tracking_config_a, std::chrono::milliseconds block_processor_batch_max_time_a, nano::lmdb_config const & lmdb_config_a, size_t const batch_size_a, bool backup_before_upgrade_a) :
logger (logger_a),
env (error, path_a, nano::mdb_env::options::make ().set_config (lmdb_config_a).set_use_no_mem_init (true)),
mdb_txn_tracker (logger_a, txn_tracking_config_a, block_processor_batch_max_time_a),
txn_tracking_enabled (txn_tracking_config_a.enable)
{
	if (!error)
	{
		auto is_fully_upgraded (false);
		auto is_fresh_db (false);
		{
			auto transaction (tx_begin_read ());
			auto err = mdb_dbi_open (env.tx (transaction), "meta", 0, &meta);
			is_fresh_db = err != MDB_SUCCESS;
			if (err == MDB_SUCCESS)
			{
				is_fully_upgraded = (version_get (transaction) == version);
				mdb_dbi_close (env, meta);
			}
		}

		// Only open a write lock when upgrades are needed. This is because CLI commands
		// open inactive nodes which can otherwise be locked here if there is a long write
		// (can be a few minutes with the --fast_bootstrap flag for instance)
		if (!is_fully_upgraded)
		{
			nano::network_constants network_constants;
			if (!is_fresh_db)
			{
				if (!network_constants.is_test_network ())
				{
					std::cout << "Upgrade in progress..." << std::endl;
				}
				if (backup_before_upgrade_a)
				{
					create_backup_file (env, path_a, logger_a);
				}
			}
			auto needs_vacuuming = false;
			{
				auto transaction (tx_begin_write ());
				open_databases (error, transaction, MDB_CREATE);
				if (!error)
				{
					error |= do_upgrades (transaction, needs_vacuuming, batch_size_a);
				}
			}

			if (needs_vacuuming && !network_constants.is_test_network ())
			{
				logger.always_log ("Preparing vacuum...");
				auto vacuum_success = vacuum_after_upgrade (path_a, lmdb_config_a);
				logger.always_log (vacuum_success ? "Vacuum succeeded." : "Failed to vacuum. (Optional) Ensure enough disk space is available for a copy of the database and try to vacuum after shutting down the node");
			}
		}
		else
		{
			auto transaction (tx_begin_read ());
			open_databases (error, transaction, 0);
		}
	}
}

bool nano::mdb_store::vacuum_after_upgrade (boost::filesystem::path const & path_a, nano::lmdb_config const & lmdb_config_a)
{
	// Vacuum the database. This is not a required step and may actually fail if there isn't enough storage space.
	auto vacuum_path = path_a.parent_path () / "vacuumed.ldb";

	auto vacuum_success = copy_db (vacuum_path);
	if (vacuum_success)
	{
		// Need to close the database to release the file handle
		mdb_env_sync (env.environment, true);
		mdb_env_close (env.environment);
		env.environment = nullptr;

		// Replace the ledger file with the vacuumed one
		boost::filesystem::rename (vacuum_path, path_a);

		// Set up the environment again
		auto options = nano::mdb_env::options::make ()
		               .set_config (lmdb_config_a)
		               .set_use_no_mem_init (true);
		env.init (error, path_a, options);
		if (!error)
		{
			auto transaction (tx_begin_read ());
			open_databases (error, transaction, 0);
		}
	}
	else
	{
		// The vacuum file can be in an inconsistent state if there wasn't enough space to create it
		boost::filesystem::remove (vacuum_path);
	}
	return vacuum_success;
}

void nano::mdb_store::serialize_mdb_tracker (boost::property_tree::ptree & json, std::chrono::milliseconds min_read_time, std::chrono::milliseconds min_write_time)
{
	mdb_txn_tracker.serialize_json (json, min_read_time, min_write_time);
}

nano::write_transaction nano::mdb_store::tx_begin_write (std::vector<nano::tables> const &, std::vector<nano::tables> const &)
{
	return env.tx_begin_write (create_txn_callbacks ());
}

nano::read_transaction nano::mdb_store::tx_begin_read ()
{
	return env.tx_begin_read (create_txn_callbacks ());
}

std::string nano::mdb_store::vendor_get () const
{
	return boost::str (boost::format ("LMDB %1%.%2%.%3%") % MDB_VERSION_MAJOR % MDB_VERSION_MINOR % MDB_VERSION_PATCH);
}

nano::mdb_txn_callbacks nano::mdb_store::create_txn_callbacks ()
{
	nano::mdb_txn_callbacks mdb_txn_callbacks;
	if (txn_tracking_enabled)
	{
		mdb_txn_callbacks.txn_start = ([& mdb_txn_tracker = mdb_txn_tracker](const nano::transaction_impl * transaction_impl) {
			mdb_txn_tracker.add (transaction_impl);
		});
		mdb_txn_callbacks.txn_end = ([& mdb_txn_tracker = mdb_txn_tracker](const nano::transaction_impl * transaction_impl) {
			mdb_txn_tracker.erase (transaction_impl);
		});
	}
	return mdb_txn_callbacks;
}

void nano::mdb_store::open_databases (bool & error_a, nano::transaction const & transaction_a, unsigned flags)
{
	error_a |= mdb_dbi_open (env.tx (transaction_a), "frontiers", flags, &frontiers) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "send", flags, &send_blocks) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "receive", flags, &receive_blocks) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "open", flags, &open_blocks) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "change", flags, &change_blocks) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "unchecked", flags, &unchecked) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "vote", flags, &vote) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "online_weight", flags, &online_weight) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "meta", flags, &meta) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "peers", flags, &peers) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "confirmation_height", flags, &confirmation_height) != 0;
	if (!full_sideband (transaction_a))
	{
		// The blocks_info database is no longer used, but need opening so that it can be deleted during an upgrade
		error_a |= mdb_dbi_open (env.tx (transaction_a), "blocks_info", flags, &blocks_info) != 0;
	}
	error_a |= mdb_dbi_open (env.tx (transaction_a), "accounts", flags, &accounts_v0) != 0;
	accounts = accounts_v0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "pending", flags, &pending_v0) != 0;
	pending = pending_v0;

	if (version_get (transaction_a) < 16)
	{
		// The representation database is no longer used, but needs opening so that it can be deleted during an upgrade
		error_a |= mdb_dbi_open (env.tx (transaction_a), "representation", flags, &representation) != 0;
	}

	if (version_get (transaction_a) < 15)
	{
		// These databases are no longer used, but need opening so they can be deleted during an upgrade
		error_a |= mdb_dbi_open (env.tx (transaction_a), "state", flags, &state_blocks_v0) != 0;
		state_blocks = state_blocks_v0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "accounts_v1", flags, &accounts_v1) != 0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "pending_v1", flags, &pending_v1) != 0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "state_v1", flags, &state_blocks_v1) != 0;
	}
	else
	{
		error_a |= mdb_dbi_open (env.tx (transaction_a), "state_blocks", flags, &state_blocks) != 0;
		state_blocks_v0 = state_blocks;
	}
}

bool nano::mdb_store::do_upgrades (nano::write_transaction & transaction_a, bool & needs_vacuuming, size_t batch_size_a)
{
	auto error (false);
	auto version_l = version_get (transaction_a);
	switch (version_l)
	{
		case 1:
			upgrade_v1_to_v2 (transaction_a);
		case 2:
			upgrade_v2_to_v3 (transaction_a);
		case 3:
			upgrade_v3_to_v4 (transaction_a);
		case 4:
			upgrade_v4_to_v5 (transaction_a);
		case 5:
			upgrade_v5_to_v6 (transaction_a);
		case 6:
			upgrade_v6_to_v7 (transaction_a);
		case 7:
			upgrade_v7_to_v8 (transaction_a);
		case 8:
			upgrade_v8_to_v9 (transaction_a);
		case 9:
		case 10:
			upgrade_v10_to_v11 (transaction_a);
		case 11:
			upgrade_v11_to_v12 (transaction_a);
		case 12:
			upgrade_v12_to_v13 (transaction_a, batch_size_a);
		case 13:
			upgrade_v13_to_v14 (transaction_a);
		case 14:
			upgrade_v14_to_v15 (transaction_a);
			needs_vacuuming = true;
		case 15:
			// Upgrades to v16, v17 & v18 are all part of the v21 node release
			upgrade_v15_to_v16 (transaction_a);
		case 16:
			upgrade_v16_to_v17 (transaction_a);
		case 17:
			upgrade_v17_to_v18 (transaction_a);
			needs_vacuuming = true;
		case 18:
			break;
		default:
			logger.always_log (boost::str (boost::format ("The version of the ledger (%1%) is too high for this node") % version_l));
			error = true;
			break;
	}
	return error;
}

void nano::mdb_store::upgrade_v1_to_v2 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 2);
	nano::account account (1);
	while (!account.is_zero ())
	{
		nano::mdb_iterator<nano::account, nano::account_info_v1> i (transaction_a, accounts_v0, nano::mdb_val (account));
		std::cerr << std::hex;
		if (i != nano::mdb_iterator<nano::account, nano::account_info_v1>{})
		{
			account = nano::account (i->first);
			nano::account_info_v1 v1 (i->second);
			nano::account_info_v5 v2;
			v2.balance = v1.balance;
			v2.head = v1.head;
			v2.modified = v1.modified;
			v2.rep_block = v1.rep_block;
			auto block (block_get (transaction_a, v1.head));
			while (!block->previous ().is_zero ())
			{
				block = block_get (transaction_a, block->previous ());
			}
			v2.open_block = block->hash ();
			auto status (mdb_put (env.tx (transaction_a), accounts_v0, nano::mdb_val (account), nano::mdb_val (sizeof (v2), &v2), 0));
			release_assert (status == 0);
			account = account.number () + 1;
		}
		else
		{
			account.clear ();
		}
	}
}

void nano::mdb_store::upgrade_v2_to_v3 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 3);
	mdb_drop (env.tx (transaction_a), representation, 0);
	for (auto i (std::make_unique<nano::mdb_iterator<nano::account, nano::account_info_v5>> (transaction_a, accounts_v0)), n (std::make_unique<nano::mdb_iterator<nano::account, nano::account_info_v5>> ()); *i != *n; ++(*i))
	{
		nano::account account_l ((*i)->first);
		nano::account_info_v5 info ((*i)->second);
		representative_visitor visitor (transaction_a, *this);
		visitor.compute (info.head);
		debug_assert (!visitor.result.is_zero ());
		info.rep_block = visitor.result;
		auto impl (boost::polymorphic_downcast<nano::mdb_iterator<nano::account, nano::account_info_v5> *> (i.get ()));
		mdb_cursor_put (impl->cursor, nano::mdb_val (account_l), nano::mdb_val (sizeof (info), &info), MDB_CURRENT);
	}
}

void nano::mdb_store::upgrade_v3_to_v4 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 4);
	std::queue<std::pair<nano::pending_key, nano::pending_info_v14>> items;
	for (auto i (nano::store_iterator<nano::block_hash, nano::pending_info_v3> (std::make_unique<nano::mdb_iterator<nano::block_hash, nano::pending_info_v3>> (transaction_a, pending_v0))), n (nano::store_iterator<nano::block_hash, nano::pending_info_v3> (nullptr)); i != n; ++i)
	{
		nano::block_hash const & hash (i->first);
		nano::pending_info_v3 const & info (i->second);
		items.emplace (nano::pending_key (info.destination, hash), nano::pending_info_v14 (info.source, info.amount, nano::epoch::epoch_0));
	}
	mdb_drop (env.tx (transaction_a), pending_v0, 0);
	while (!items.empty ())
	{
		auto status (mdb_put (env.tx (transaction_a), pending, nano::mdb_val (items.front ().first), nano::mdb_val (items.front ().second), 0));
		debug_assert (success (status));
		items.pop ();
	}
}

void nano::mdb_store::upgrade_v4_to_v5 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 5);
	for (auto i (nano::store_iterator<nano::account, nano::account_info_v5> (std::make_unique<nano::mdb_iterator<nano::account, nano::account_info_v5>> (transaction_a, accounts_v0))), n (nano::store_iterator<nano::account, nano::account_info_v5> (nullptr)); i != n; ++i)
	{
		nano::account_info_v5 const & info (i->second);
		nano::block_hash successor (0);
		auto block (block_get (transaction_a, info.head));
		while (block != nullptr)
		{
			auto hash (block->hash ());
			if (block_successor (transaction_a, hash).is_zero () && !successor.is_zero ())
			{
				std::vector<uint8_t> vector;
				{
					nano::vectorstream stream (vector);
					block->serialize (stream);
					nano::write (stream, successor.bytes);
				}
				block_raw_put (transaction_a, vector, block->type (), hash);
				if (!block->previous ().is_zero ())
				{
					nano::block_type type;
					auto value (block_raw_get (transaction_a, block->previous (), type));
					debug_assert (value.size () != 0);
					std::vector<uint8_t> data (static_cast<uint8_t *> (value.data ()), static_cast<uint8_t *> (value.data ()) + value.size ());
					std::copy (hash.bytes.begin (), hash.bytes.end (), data.end () - nano::block_sideband::size (type));
					block_raw_put (transaction_a, data, type, block->previous ());
				}
			}
			successor = hash;
			block = block_get (transaction_a, block->previous ());
		}
	}
}

void nano::mdb_store::upgrade_v5_to_v6 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 6);
	std::deque<std::pair<nano::account, nano::account_info_v13>> headers;
	for (auto i (nano::store_iterator<nano::account, nano::account_info_v5> (std::make_unique<nano::mdb_iterator<nano::account, nano::account_info_v5>> (transaction_a, accounts_v0))), n (nano::store_iterator<nano::account, nano::account_info_v5> (nullptr)); i != n; ++i)
	{
		nano::account const & account (i->first);
		nano::account_info_v5 info_old (i->second);
		uint64_t block_count (0);
		auto hash (info_old.head);
		while (!hash.is_zero ())
		{
			++block_count;
			auto block (block_get (transaction_a, hash));
			debug_assert (block != nullptr);
			hash = block->previous ();
		}
		headers.emplace_back (account, nano::account_info_v13{ info_old.head, info_old.rep_block, info_old.open_block, info_old.balance, info_old.modified, block_count, nano::epoch::epoch_0 });
	}
	for (auto i (headers.begin ()), n (headers.end ()); i != n; ++i)
	{
		auto status (mdb_put (env.tx (transaction_a), accounts_v0, nano::mdb_val (i->first), nano::mdb_val (i->second), 0));
		release_assert (status == 0);
	}
}

void nano::mdb_store::upgrade_v6_to_v7 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 7);
	mdb_drop (env.tx (transaction_a), unchecked, 0);
}

void nano::mdb_store::upgrade_v7_to_v8 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 8);
	mdb_drop (env.tx (transaction_a), unchecked, 1);
	mdb_dbi_open (env.tx (transaction_a), "unchecked", MDB_CREATE | MDB_DUPSORT, &unchecked);
}

void nano::mdb_store::upgrade_v8_to_v9 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 9);
	MDB_dbi sequence;
	mdb_dbi_open (env.tx (transaction_a), "sequence", MDB_CREATE | MDB_DUPSORT, &sequence);
	nano::genesis genesis;
	std::shared_ptr<nano::block> block (std::move (genesis.open));
	nano::keypair junk;
	for (nano::mdb_iterator<nano::account, uint64_t> i (transaction_a, sequence), n (nano::mdb_iterator<nano::account, uint64_t>{}); i != n; ++i)
	{
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (i->second.data ()), i->second.size ());
		uint64_t sequence;
		auto error (nano::try_read (stream, sequence));
		(void)error;
		// Create a dummy vote with the same sequence number for easy upgrading.  This won't have a valid signature.
		nano::vote dummy (nano::account (i->first), junk.prv, sequence, block);
		std::vector<uint8_t> vector;
		{
			nano::vectorstream stream (vector);
			dummy.serialize (stream);
		}
		auto status1 (mdb_put (env.tx (transaction_a), vote, nano::mdb_val (i->first), nano::mdb_val (vector.size (), vector.data ()), 0));
		release_assert (status1 == 0);
		debug_assert (!error);
	}
	mdb_drop (env.tx (transaction_a), sequence, 1);
}

void nano::mdb_store::upgrade_v10_to_v11 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 11);
	MDB_dbi unsynced;
	mdb_dbi_open (env.tx (transaction_a), "unsynced", MDB_CREATE | MDB_DUPSORT, &unsynced);
	mdb_drop (env.tx (transaction_a), unsynced, 1);
}

void nano::mdb_store::upgrade_v11_to_v12 (nano::write_transaction const & transaction_a)
{
	version_put (transaction_a, 12);
	mdb_drop (env.tx (transaction_a), unchecked, 1);
	mdb_dbi_open (env.tx (transaction_a), "unchecked", MDB_CREATE, &unchecked);
	MDB_dbi checksum;
	mdb_dbi_open (env.tx (transaction_a), "checksum", MDB_CREATE, &checksum);
	mdb_drop (env.tx (transaction_a), checksum, 1);
}

void nano::mdb_store::upgrade_v12_to_v13 (nano::write_transaction & transaction_a, size_t const batch_size)
{
	size_t cost (0);
	nano::account account (0);
	auto const & not_an_account (network_params.random.not_an_account);
	while (account != not_an_account)
	{
		nano::account first (0);
		nano::account_info_v13 second;
		{
			nano::mdb_merge_iterator<nano::account, nano::account_info_v13> current (transaction_a, accounts_v0, accounts_v1, nano::mdb_val (account));
			nano::mdb_merge_iterator<nano::account, nano::account_info_v13> end{};
			if (current != end)
			{
				first = nano::account (current->first);
				second = nano::account_info_v13 (current->second);
			}
		}
		if (!first.is_zero ())
		{
			auto hash (second.open_block);
			uint64_t height (1);
			nano::block_sideband_v14 sideband;
			while (!hash.is_zero ())
			{
				if (cost >= batch_size)
				{
					logger.always_log (boost::str (boost::format ("Upgrading sideband information for account %1%... height %2%") % first.to_account ().substr (0, 24) % std::to_string (height)));
					transaction_a.commit ();
					std::this_thread::yield ();
					transaction_a.renew ();
					cost = 0;
				}

				bool is_state_block_v1 = false;
				auto block = block_get_v14 (transaction_a, hash, &sideband, &is_state_block_v1);

				debug_assert (block != nullptr);
				if (sideband.height == 0)
				{
					sideband.height = height;

					std::vector<uint8_t> vector;
					{
						nano::vectorstream stream (vector);
						block->serialize (stream);
						sideband.serialize (stream);
					}

					nano::mdb_val value{ vector.size (), (void *)vector.data () };
					MDB_dbi database = is_state_block_v1 ? state_blocks_v1 : table_to_dbi (block_database (block->type ()));

					auto status = mdb_put (env.tx (transaction_a), database, nano::mdb_val (hash), value, 0);
					release_assert (success (status));

					nano::block_predecessor_set<MDB_val, nano::mdb_store> predecessor (transaction_a, *this);
					block->visit (predecessor);
					debug_assert (block->previous ().is_zero () || block_successor (transaction_a, block->previous ()) == hash);
					cost += 16;
				}
				else
				{
					cost += 1;
				}
				hash = sideband.successor;
				++height;
			}
			account = first.number () + 1;
		}
		else
		{
			account = not_an_account;
		}
	}
	if (account == not_an_account)
	{
		logger.always_log ("Completed sideband upgrade");
		version_put (transaction_a, 13);
	}
}

void nano::mdb_store::upgrade_v13_to_v14 (nano::write_transaction const & transaction_a)
{
	// Upgrade all accounts to have a confirmation of 0 (except genesis which should have 1)
	version_put (transaction_a, 14);
	nano::mdb_merge_iterator<nano::account, nano::account_info_v13> i (transaction_a, accounts_v0, accounts_v1);
	nano::mdb_merge_iterator<nano::account, nano::account_info_v13> n{};
	std::vector<std::pair<nano::account, nano::account_info_v14>> account_infos;
	account_infos.reserve (count (transaction_a, accounts_v0) + count (transaction_a, accounts_v1));
	for (; i != n; ++i)
	{
		nano::account account (i->first);
		nano::account_info_v13 account_info_v13 (i->second);

		uint64_t confirmation_height = 0;
		if (account == network_params.ledger.genesis_account)
		{
			confirmation_height = 1;
		}
		account_infos.emplace_back (account, nano::account_info_v14{ account_info_v13.head, account_info_v13.rep_block, account_info_v13.open_block, account_info_v13.balance, account_info_v13.modified, account_info_v13.block_count, confirmation_height, i.from_first_database ? nano::epoch::epoch_0 : nano::epoch::epoch_1 });
	}

	for (auto const & account_info : account_infos)
	{
		auto status1 (mdb_put (env.tx (transaction_a), account_info.second.epoch == nano::epoch::epoch_0 ? accounts_v0 : accounts_v1, nano::mdb_val (account_info.first), nano::mdb_val (account_info.second), 0));
		release_assert (status1 == 0);
	}

	logger.always_log ("Completed confirmation height upgrade");

	nano::uint256_union node_id_mdb_key (3);
	auto error (mdb_del (env.tx (transaction_a), meta, nano::mdb_val (node_id_mdb_key), nullptr));
	release_assert (!error || error == MDB_NOTFOUND);
}

void nano::mdb_store::upgrade_v14_to_v15 (nano::write_transaction & transaction_a)
{
	logger.always_log ("Preparing v14 to v15 database upgrade...");

	std::vector<std::pair<nano::account, nano::account_info>> account_infos;
	upgrade_counters account_counters (count (transaction_a, accounts_v0), count (transaction_a, accounts_v1));
	account_infos.reserve (account_counters.before_v0 + account_counters.before_v1);

	nano::mdb_merge_iterator<nano::account, nano::account_info_v14> i_account (transaction_a, accounts_v0, accounts_v1);
	nano::mdb_merge_iterator<nano::account, nano::account_info_v14> n_account{};
	for (; i_account != n_account; ++i_account)
	{
		nano::account account (i_account->first);
		nano::account_info_v14 account_info_v14 (i_account->second);

		// Upgrade rep block to representative account
		auto rep_block = block_get_v14 (transaction_a, account_info_v14.rep_block);
		release_assert (rep_block != nullptr);
		account_infos.emplace_back (account, nano::account_info{ account_info_v14.head, rep_block->representative (), account_info_v14.open_block, account_info_v14.balance, account_info_v14.modified, account_info_v14.block_count, i_account.from_first_database ? nano::epoch::epoch_0 : nano::epoch::epoch_1 });
		// Move confirmation height from account_info database to its own table
		mdb_put (env.tx (transaction_a), confirmation_height, nano::mdb_val (account), nano::mdb_val (account_info_v14.confirmation_height), MDB_APPEND);
		i_account.from_first_database ? ++account_counters.after_v0 : ++account_counters.after_v1;
	}

	logger.always_log ("Finished extracting confirmation height to its own database");

	debug_assert (account_counters.are_equal ());
	// No longer need accounts_v1, keep v0 but clear it
	mdb_drop (env.tx (transaction_a), accounts_v1, 1);
	mdb_drop (env.tx (transaction_a), accounts_v0, 0);

	for (auto const & account_account_info_pair : account_infos)
	{
		auto const & account_info (account_account_info_pair.second);
		mdb_put (env.tx (transaction_a), accounts, nano::mdb_val (account_account_info_pair.first), nano::mdb_val (account_info), MDB_APPEND);
	}

	logger.always_log ("Epoch merge upgrade: Finished accounts, now doing state blocks");

	account_infos.clear ();

	// Have to create a new database as we are iterating over the existing ones and want to use MDB_APPEND for quick insertion
	MDB_dbi state_blocks_new;
	mdb_dbi_open (env.tx (transaction_a), "state_blocks", MDB_CREATE, &state_blocks_new);

	upgrade_counters state_counters (count (transaction_a, state_blocks_v0), count (transaction_a, state_blocks_v1));

	nano::mdb_merge_iterator<nano::block_hash, nano::state_block_w_sideband_v14> i_state (transaction_a, state_blocks_v0, state_blocks_v1);
	nano::mdb_merge_iterator<nano::block_hash, nano::state_block_w_sideband_v14> n_state{};
	auto num = 0u;
	for (; i_state != n_state; ++i_state, ++num)
	{
		nano::block_hash hash (i_state->first);
		nano::state_block_w_sideband_v14 state_block_w_sideband_v14 (i_state->second);
		auto & sideband_v14 = state_block_w_sideband_v14.sideband;

		nano::block_sideband sideband (sideband_v14.account, sideband_v14.successor, sideband_v14.balance, sideband_v14.height, sideband_v14.timestamp, i_state.from_first_database ? nano::epoch::epoch_0 : nano::epoch::epoch_1, false, false, false);

		// Write these out
		std::vector<uint8_t> data;
		{
			nano::vectorstream stream (data);
			state_block_w_sideband_v14.state_block->serialize (stream);
			sideband.serialize (stream, sideband_v14.type);
		}

		nano::mdb_val value{ data.size (), (void *)data.data () };
		auto s = mdb_put (env.tx (transaction_a), state_blocks_new, nano::mdb_val (hash), value, MDB_APPEND);
		release_assert (success (s));

		// Every so often output to the log to indicate progress
		constexpr auto output_cutoff = 1000000;
		if (num % output_cutoff == 0 && num != 0)
		{
			logger.always_log (boost::str (boost::format ("Database epoch merge upgrade %1% million state blocks upgraded") % (num / output_cutoff)));
		}
		i_state.from_first_database ? ++state_counters.after_v0 : ++state_counters.after_v1;
	}

	debug_assert (state_counters.are_equal ());
	logger.always_log ("Epoch merge upgrade: Finished state blocks, now doing pending blocks");

	state_blocks = state_blocks_new;

	// No longer need states v0/v1 databases
	mdb_drop (env.tx (transaction_a), state_blocks_v1, 1);
	mdb_drop (env.tx (transaction_a), state_blocks_v0, 1);

	state_blocks_v0 = state_blocks;

	upgrade_counters pending_counters (count (transaction_a, pending_v0), count (transaction_a, pending_v1));
	std::vector<std::pair<nano::pending_key, nano::pending_info>> pending_infos;
	pending_infos.reserve (pending_counters.before_v0 + pending_counters.before_v1);

	nano::mdb_merge_iterator<nano::pending_key, nano::pending_info_v14> i_pending (transaction_a, pending_v0, pending_v1);
	nano::mdb_merge_iterator<nano::pending_key, nano::pending_info_v14> n_pending{};
	for (; i_pending != n_pending; ++i_pending)
	{
		nano::pending_info_v14 info (i_pending->second);
		pending_infos.emplace_back (nano::pending_key (i_pending->first), nano::pending_info{ info.source, info.amount, i_pending.from_first_database ? nano::epoch::epoch_0 : nano::epoch::epoch_1 });
		i_pending.from_first_database ? ++pending_counters.after_v0 : ++pending_counters.after_v1;
	}

	debug_assert (pending_counters.are_equal ());

	// No longer need the pending v1 table
	mdb_drop (env.tx (transaction_a), pending_v1, 1);
	mdb_drop (env.tx (transaction_a), pending_v0, 0);

	for (auto const & pending_key_pending_info_pair : pending_infos)
	{
		mdb_put (env.tx (transaction_a), pending, nano::mdb_val (pending_key_pending_info_pair.first), nano::mdb_val (pending_key_pending_info_pair.second), MDB_APPEND);
	}

	version_put (transaction_a, 15);
	logger.always_log ("Finished epoch merge upgrade");
}

void nano::mdb_store::upgrade_v15_to_v16 (nano::write_transaction const & transaction_a)
{
	// Representation table is no longer used
	debug_assert (representation != 0);
	if (representation != 0)
	{
		auto status (mdb_drop (env.tx (transaction_a), representation, 1));
		release_assert (status == MDB_SUCCESS);
		representation = 0;
	}
	version_put (transaction_a, 16);
}

void nano::mdb_store::upgrade_v16_to_v17 (nano::write_transaction const & transaction_a)
{
	logger.always_log ("Preparing v16 to v17 database upgrade...");

	auto account_info_i = latest_begin (transaction_a);
	auto account_info_n = latest_end ();

	// Set the confirmed frontier for each account in the confirmation height table
	std::vector<std::pair<nano::account, nano::confirmation_height_info>> confirmation_height_infos;
	auto num = 0u;
	for (nano::mdb_iterator<nano::account, uint64_t> i (transaction_a, confirmation_height), n (nano::mdb_iterator<nano::account, uint64_t>{}); i != n; ++i, ++account_info_i, ++num)
	{
		nano::account account (i->first);
		uint64_t confirmation_height (i->second);

		// Check account hashes matches both the accounts table and confirmation height table
		debug_assert (account == account_info_i->first);

		auto const & account_info = account_info_i->second;

		if (confirmation_height == 0)
		{
			confirmation_height_infos.emplace_back (account, confirmation_height_info{ 0, nano::block_hash (0) });
		}
		else
		{
			if (account_info_i->second.block_count / 2 >= confirmation_height)
			{
				// The confirmation height of the account is closer to the bottom of the chain, so start there and work up
				auto block = block_get (transaction_a, account_info.open_block);
				debug_assert (block);
				auto height = 1;

				while (height != confirmation_height)
				{
					block = block_get (transaction_a, block->sideband ().successor);
					debug_assert (block);
					++height;
				}

				debug_assert (block->sideband ().height == confirmation_height);
				confirmation_height_infos.emplace_back (account, confirmation_height_info{ confirmation_height, block->hash () });
			}
			else
			{
				// The confirmation height of the account is closer to the top of the chain so start there and work down
				auto block = block_get (transaction_a, account_info.head);
				auto height = block->sideband ().height;
				while (height != confirmation_height)
				{
					block = block_get (transaction_a, block->previous ());
					debug_assert (block);
					--height;
				}
				confirmation_height_infos.emplace_back (account, confirmation_height_info{ confirmation_height, block->hash () });
			}
		}

		// Every so often output to the log to indicate progress (every 200k accounts)
		constexpr auto output_cutoff = 200000;
		if (num % output_cutoff == 0 && num != 0)
		{
			logger.always_log (boost::str (boost::format ("Confirmation height frontier set for %1%00k accounts") % ((num / output_cutoff) * 2)));
		}
	}

	// Clear it then append
	auto status (mdb_drop (env.tx (transaction_a), confirmation_height, 0));
	release_assert (status == MDB_SUCCESS);

	for (auto const & confirmation_height_info_pair : confirmation_height_infos)
	{
		mdb_put (env.tx (transaction_a), confirmation_height, nano::mdb_val (confirmation_height_info_pair.first), nano::mdb_val (confirmation_height_info_pair.second), MDB_APPEND);
	}

	version_put (transaction_a, 17);
	logger.always_log ("Finished upgrading confirmation height frontiers");
}

void nano::mdb_store::upgrade_v17_to_v18 (nano::write_transaction const & transaction_a)
{
	logger.always_log ("Preparing v17 to v18 database upgrade...");

	auto count_pre (count (transaction_a, state_blocks));

	auto num = 0u;
	for (nano::mdb_iterator<nano::block_hash, nano::state_block_w_sideband> state_i (transaction_a, state_blocks), state_n{}; state_i != state_n; ++state_i, ++num)
	{
		nano::state_block_w_sideband block_sideband (state_i->second);
		auto & block (block_sideband.state_block);
		auto & sideband (block_sideband.sideband);

		bool is_send{ false };
		bool is_receive{ false };
		bool is_epoch{ false };

		nano::amount prev_balance (0);
		if (!block->hashables.previous.is_zero ())
		{
			prev_balance = block_balance (transaction_a, block->hashables.previous);
		}
		if (block->hashables.balance == prev_balance && network_params.ledger.epochs.is_epoch_link (block->hashables.link))
		{
			is_epoch = true;
		}
		else if (block->hashables.balance < prev_balance)
		{
			is_send = true;
		}
		else if (!block->hashables.link.is_zero ())
		{
			is_receive = true;
		}

		nano::block_sideband new_sideband (sideband.account, sideband.successor, sideband.balance, sideband.height, sideband.timestamp, sideband.details.epoch, is_send, is_receive, is_epoch);
		// Write these out
		std::vector<uint8_t> data;
		{
			nano::vectorstream stream (data);
			block->serialize (stream);
			new_sideband.serialize (stream, block->type ());
		}
		nano::mdb_val value{ data.size (), (void *)data.data () };
		auto s = mdb_cursor_put (state_i.cursor, state_i->first, value, MDB_CURRENT);
		release_assert (success (s));

		// Every so often output to the log to indicate progress
		constexpr auto output_cutoff = 1000000;
		if (num > 0 && num % output_cutoff == 0)
		{
			logger.always_log (boost::str (boost::format ("Database sideband upgrade %1% million state blocks upgraded (out of %2%)") % (num / output_cutoff) % count_pre));
		}
	}

	auto count_post (count (transaction_a, state_blocks));
	release_assert (count_pre == count_post);

	version_put (transaction_a, 18);
	logger.always_log ("Finished upgrading the sideband");
}

/** Takes a filepath, appends '_backup_<timestamp>' to the end (but before any extension) and saves that file in the same directory */
void nano::mdb_store::create_backup_file (nano::mdb_env & env_a, boost::filesystem::path const & filepath_a, nano::logger_mt & logger_a)
{
	auto extension = filepath_a.extension ();
	auto filename_without_extension = filepath_a.filename ().replace_extension ("");
	auto orig_filepath = filepath_a;
	auto & backup_path = orig_filepath.remove_filename ();
	auto backup_filename = filename_without_extension;
	backup_filename += "_backup_";
	backup_filename += std::to_string (std::chrono::system_clock::now ().time_since_epoch ().count ());
	backup_filename += extension;
	auto backup_filepath = backup_path / backup_filename;
	auto start_message (boost::str (boost::format ("Performing %1% backup before database upgrade...") % filepath_a.filename ()));
	logger_a.always_log (start_message);
	std::cout << start_message << std::endl;
	auto error (mdb_env_copy (env_a, backup_filepath.string ().c_str ()));
	if (error)
	{
		auto error_message (boost::str (boost::format ("%1% backup failed") % filepath_a.filename ()));
		logger_a.always_log (error_message);
		std::cerr << error_message << std::endl;
		std::exit (1);
	}
	else
	{
		auto success_message (boost::str (boost::format ("Backup created: %1%") % backup_filename));
		logger_a.always_log (success_message);
		std::cout << success_message << std::endl;
	}
}

void nano::mdb_store::version_put (nano::write_transaction const & transaction_a, int version_a)
{
	nano::uint256_union version_key (1);
	nano::uint256_union version_value (version_a);
	auto status (mdb_put (env.tx (transaction_a), meta, nano::mdb_val (version_key), nano::mdb_val (version_value), 0));
	release_assert (status == 0);
	if (blocks_info == 0 && !full_sideband (transaction_a))
	{
		auto status (mdb_dbi_open (env.tx (transaction_a), "blocks_info", MDB_CREATE, &blocks_info));
		release_assert (status == MDB_SUCCESS);
	}
	if (blocks_info != 0 && full_sideband (transaction_a))
	{
		auto status (mdb_drop (env.tx (transaction_a), blocks_info, 1));
		release_assert (status == MDB_SUCCESS);
		blocks_info = 0;
	}
}

bool nano::mdb_store::block_info_get (nano::transaction const & transaction_a, nano::block_hash const & hash_a, nano::block_info & block_info_a) const
{
	debug_assert (!full_sideband (transaction_a));
	nano::mdb_val value;
	auto status (mdb_get (env.tx (transaction_a), blocks_info, nano::mdb_val (hash_a), value));
	release_assert (status == 0 || status == MDB_NOTFOUND);
	bool result (true);
	if (status != MDB_NOTFOUND)
	{
		result = false;
		debug_assert (value.size () == sizeof (block_info_a.account.bytes) + sizeof (block_info_a.balance.bytes));
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		auto error1 (nano::try_read (stream, block_info_a.account));
		(void)error1;
		debug_assert (!error1);
		auto error2 (nano::try_read (stream, block_info_a.balance));
		(void)error2;
		debug_assert (!error2);
	}
	return result;
}

bool nano::mdb_store::exists (nano::transaction const & transaction_a, tables table_a, nano::mdb_val const & key_a) const
{
	nano::mdb_val junk;
	auto status = get (transaction_a, table_a, key_a, junk);
	release_assert (status == MDB_SUCCESS || status == MDB_NOTFOUND);
	return (status == MDB_SUCCESS);
}

int nano::mdb_store::get (nano::transaction const & transaction_a, tables table_a, nano::mdb_val const & key_a, nano::mdb_val & value_a) const
{
	return mdb_get (env.tx (transaction_a), table_to_dbi (table_a), key_a, value_a);
}

int nano::mdb_store::put (nano::write_transaction const & transaction_a, tables table_a, nano::mdb_val const & key_a, const nano::mdb_val & value_a) const
{
	return (mdb_put (env.tx (transaction_a), table_to_dbi (table_a), key_a, value_a, 0));
}

int nano::mdb_store::del (nano::write_transaction const & transaction_a, tables table_a, nano::mdb_val const & key_a) const
{
	return (mdb_del (env.tx (transaction_a), table_to_dbi (table_a), key_a, nullptr));
}

int nano::mdb_store::drop (nano::write_transaction const & transaction_a, tables table_a)
{
	return clear (transaction_a, table_to_dbi (table_a));
}

int nano::mdb_store::clear (nano::write_transaction const & transaction_a, MDB_dbi handle_a)
{
	return mdb_drop (env.tx (transaction_a), handle_a, 0);
}

size_t nano::mdb_store::count (nano::transaction const & transaction_a, tables table_a) const
{
	return count (transaction_a, table_to_dbi (table_a));
}

size_t nano::mdb_store::count (nano::transaction const & transaction_a, MDB_dbi db_a) const
{
	MDB_stat stats;
	auto status (mdb_stat (env.tx (transaction_a), db_a, &stats));
	release_assert (status == 0);
	return (stats.ms_entries);
}

MDB_dbi nano::mdb_store::table_to_dbi (tables table_a) const
{
	switch (table_a)
	{
		case tables::frontiers:
			return frontiers;
		case tables::accounts:
			return accounts;
		case tables::send_blocks:
			return send_blocks;
		case tables::receive_blocks:
			return receive_blocks;
		case tables::open_blocks:
			return open_blocks;
		case tables::change_blocks:
			return change_blocks;
		case tables::state_blocks:
			return state_blocks;
		case tables::pending:
			return pending;
		case tables::blocks_info:
			return blocks_info;
		case tables::unchecked:
			return unchecked;
		case tables::vote:
			return vote;
		case tables::online_weight:
			return online_weight;
		case tables::meta:
			return meta;
		case tables::peers:
			return peers;
		case tables::confirmation_height:
			return confirmation_height;
		default:
			release_assert (false);
			return peers;
	}
}

bool nano::mdb_store::not_found (int status) const
{
	return (status_code_not_found () == status);
}

bool nano::mdb_store::success (int status) const
{
	return (MDB_SUCCESS == status);
}

int nano::mdb_store::status_code_not_found () const
{
	return MDB_NOTFOUND;
}

bool nano::mdb_store::copy_db (boost::filesystem::path const & destination_file)
{
	return !mdb_env_copy2 (env.environment, destination_file.string ().c_str (), MDB_CP_COMPACT);
}

void nano::mdb_store::rebuild_db (nano::write_transaction const & transaction_a)
{
	// Tables with uint256_union key
	std::vector<MDB_dbi> tables = { accounts, send_blocks, receive_blocks, open_blocks, change_blocks, state_blocks, vote, confirmation_height };
	for (auto const & table : tables)
	{
		MDB_dbi temp;
		mdb_dbi_open (env.tx (transaction_a), "temp_table", MDB_CREATE, &temp);
		// Copy all values to temporary table
		for (auto i (nano::store_iterator<nano::uint256_union, nano::mdb_val> (std::make_unique<nano::mdb_iterator<nano::uint256_union, nano::mdb_val>> (transaction_a, table))), n (nano::store_iterator<nano::uint256_union, nano::mdb_val> (nullptr)); i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), temp, nano::mdb_val (i->first), i->second, MDB_APPEND);
			release_assert (success (s));
		}
		release_assert (count (transaction_a, table) == count (transaction_a, temp));
		// Clear existing table
		mdb_drop (env.tx (transaction_a), table, 0);
		// Put values from copy
		for (auto i (nano::store_iterator<nano::uint256_union, nano::mdb_val> (std::make_unique<nano::mdb_iterator<nano::uint256_union, nano::mdb_val>> (transaction_a, temp))), n (nano::store_iterator<nano::uint256_union, nano::mdb_val> (nullptr)); i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), table, nano::mdb_val (i->first), i->second, MDB_APPEND);
			release_assert (success (s));
		}
		release_assert (count (transaction_a, table) == count (transaction_a, temp));
		// Remove temporary table
		mdb_drop (env.tx (transaction_a), temp, 1);
	}
	// Pending table
	{
		MDB_dbi temp;
		mdb_dbi_open (env.tx (transaction_a), "temp_table", MDB_CREATE, &temp);
		// Copy all values to temporary table
		for (auto i (nano::store_iterator<nano::pending_key, nano::pending_info> (std::make_unique<nano::mdb_iterator<nano::pending_key, nano::pending_info>> (transaction_a, pending))), n (nano::store_iterator<nano::pending_key, nano::pending_info> (nullptr)); i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), temp, nano::mdb_val (i->first), nano::mdb_val (i->second), MDB_APPEND);
			release_assert (success (s));
		}
		release_assert (count (transaction_a, pending) == count (transaction_a, temp));
		mdb_drop (env.tx (transaction_a), pending, 0);
		// Put values from copy
		for (auto i (nano::store_iterator<nano::pending_key, nano::pending_info> (std::make_unique<nano::mdb_iterator<nano::pending_key, nano::pending_info>> (transaction_a, temp))), n (nano::store_iterator<nano::pending_key, nano::pending_info> (nullptr)); i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), pending, nano::mdb_val (i->first), nano::mdb_val (i->second), MDB_APPEND);
			release_assert (success (s));
		}
		release_assert (count (transaction_a, pending) == count (transaction_a, temp));
		mdb_drop (env.tx (transaction_a), temp, 1);
	}
}

bool nano::mdb_store::init_error () const
{
	return error;
}

// All the v14 functions below are only needed during upgrades
bool nano::mdb_store::entry_has_sideband_v14 (size_t entry_size_a, nano::block_type type_a) const
{
	return (entry_size_a == nano::block::size (type_a) + nano::block_sideband_v14::size (type_a));
}

size_t nano::mdb_store::block_successor_offset_v14 (nano::transaction const & transaction_a, size_t entry_size_a, nano::block_type type_a) const
{
	size_t result;
	if (full_sideband (transaction_a) || entry_has_sideband_v14 (entry_size_a, type_a))
	{
		result = entry_size_a - nano::block_sideband_v14::size (type_a);
	}
	else
	{
		// Read old successor-only sideband
		debug_assert (entry_size_a == nano::block::size (type_a) + sizeof (nano::uint256_union));
		result = entry_size_a - sizeof (nano::uint256_union);
	}
	return result;
}

nano::block_hash nano::mdb_store::block_successor_v14 (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	nano::block_type type;
	auto value (block_raw_get_v14 (transaction_a, hash_a, type));
	nano::block_hash result;
	if (value.size () != 0)
	{
		debug_assert (value.size () >= result.bytes.size ());
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()) + block_successor_offset_v14 (transaction_a, value.size (), type), result.bytes.size ());
		auto error (nano::try_read (stream, result.bytes));
		(void)error;
		debug_assert (!error);
	}
	else
	{
		result.clear ();
	}
	return result;
}

nano::mdb_val nano::mdb_store::block_raw_get_v14 (nano::transaction const & transaction_a, nano::block_hash const & hash_a, nano::block_type & type_a, bool * is_state_v1) const
{
	nano::mdb_val result;
	// Table lookups are ordered by match probability
	nano::block_type block_types[]{ nano::block_type::state, nano::block_type::send, nano::block_type::receive, nano::block_type::open, nano::block_type::change };
	for (auto current_type : block_types)
	{
		auto db_val (block_raw_get_by_type_v14 (transaction_a, hash_a, current_type, is_state_v1));
		if (db_val.is_initialized ())
		{
			type_a = current_type;
			result = db_val.get ();
			break;
		}
	}

	return result;
}

boost::optional<nano::mdb_val> nano::mdb_store::block_raw_get_by_type_v14 (nano::transaction const & transaction_a, nano::block_hash const & hash_a, nano::block_type & type_a, bool * is_state_v1) const
{
	nano::mdb_val value;
	nano::mdb_val hash (hash_a);
	int status = status_code_not_found ();
	switch (type_a)
	{
		case nano::block_type::send:
		{
			status = mdb_get (env.tx (transaction_a), send_blocks, hash, value);
			break;
		}
		case nano::block_type::receive:
		{
			status = mdb_get (env.tx (transaction_a), receive_blocks, hash, value);
			break;
		}
		case nano::block_type::open:
		{
			status = mdb_get (env.tx (transaction_a), open_blocks, hash, value);
			break;
		}
		case nano::block_type::change:
		{
			status = mdb_get (env.tx (transaction_a), change_blocks, hash, value);
			break;
		}
		case nano::block_type::state:
		{
			status = mdb_get (env.tx (transaction_a), state_blocks_v1, hash, value);
			if (is_state_v1 != nullptr)
			{
				*is_state_v1 = success (status);
			}
			if (not_found (status))
			{
				status = mdb_get (env.tx (transaction_a), state_blocks_v0, hash, value);
			}
			break;
		}
		case nano::block_type::invalid:
		case nano::block_type::not_a_block:
		{
			break;
		}
	}

	release_assert (success (status) || not_found (status));
	boost::optional<nano::mdb_val> result;
	if (success (status))
	{
		result = value;
	}
	return result;
}

nano::account nano::mdb_store::block_account_v14 (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	nano::block_sideband_v14 sideband;
	auto block (block_get_v14 (transaction_a, hash_a, &sideband));
	nano::account result (block->account ());
	if (result.is_zero ())
	{
		result = sideband.account;
	}
	debug_assert (!result.is_zero ());
	return result;
}

// Return account containing hash
nano::account nano::mdb_store::block_account_computed_v14 (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	debug_assert (!full_sideband (transaction_a));
	nano::account result (0);
	auto hash (hash_a);
	while (result.is_zero ())
	{
		auto block (block_get_v14 (transaction_a, hash));
		debug_assert (block);
		result = block->account ();
		if (result.is_zero ())
		{
			auto type (nano::block_type::invalid);
			auto value (block_raw_get_v14 (transaction_a, block->previous (), type));
			if (entry_has_sideband_v14 (value.size (), type))
			{
				result = block_account_v14 (transaction_a, block->previous ());
			}
			else
			{
				nano::block_info block_info;
				if (!block_info_get (transaction_a, hash, block_info))
				{
					result = block_info.account;
				}
				else
				{
					result = frontier_get (transaction_a, hash);
					if (result.is_zero ())
					{
						auto successor (block_successor_v14 (transaction_a, hash));
						debug_assert (!successor.is_zero ());
						hash = successor;
					}
				}
			}
		}
	}
	debug_assert (!result.is_zero ());
	return result;
}

nano::uint128_t nano::mdb_store::block_balance_computed_v14 (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	debug_assert (!full_sideband (transaction_a));
	summation_visitor visitor (transaction_a, *this, true);
	return visitor.compute_balance (hash_a);
}

std::shared_ptr<nano::block> nano::mdb_store::block_get_v14 (nano::transaction const & transaction_a, nano::block_hash const & hash_a, nano::block_sideband_v14 * sideband_a, bool * is_state_v1) const
{
	nano::block_type type;
	auto value (block_raw_get_v14 (transaction_a, hash_a, type, is_state_v1));
	std::shared_ptr<nano::block> result;
	if (value.size () != 0)
	{
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = nano::deserialize_block (stream, type);
		debug_assert (result != nullptr);
		if (sideband_a)
		{
			sideband_a->type = type;
			if (full_sideband (transaction_a) || entry_has_sideband_v14 (value.size (), type))
			{
				bool error = sideband_a->deserialize (stream);
				(void)error;
				debug_assert (!error);
			}
			else
			{
				// Reconstruct sideband data for block.
				sideband_a->account = block_account_computed_v14 (transaction_a, hash_a);
				sideband_a->balance = block_balance_computed_v14 (transaction_a, hash_a);
				sideband_a->successor = block_successor_v14 (transaction_a, hash_a);
				sideband_a->height = 0;
				sideband_a->timestamp = 0;
			}
		}
	}
	return result;
}

nano::mdb_store::upgrade_counters::upgrade_counters (uint64_t count_before_v0, uint64_t count_before_v1) :
before_v0 (count_before_v0),
before_v1 (count_before_v1)
{
}

bool nano::mdb_store::upgrade_counters::are_equal () const
{
	return (before_v0 == after_v0) && (before_v1 == after_v1);
}

// Explicitly instantiate
template class nano::block_store_partial<MDB_val, nano::mdb_store>;
