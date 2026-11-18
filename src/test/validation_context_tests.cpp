// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The script flags of a block, from its hash and its deployments.

#include <consensus/params.h>
#include <script/interpreter.h>
#include <script/verify_flags.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(validation_context_tests)

namespace {
struct MainNetSetup : public TestingSetup {
    MainNetSetup() : TestingSetup{ChainType::MAIN} {}
};
} // namespace

// The script flags of a block from its hash and the deployments alone: what each deployment adds, and
// the blocks of the exception table, whose flags are the table's and what the deployments add.
BOOST_FIXTURE_TEST_CASE(script_flags_of_a_block, MainNetSetup)
{
    const Consensus::Params& params{Assert(m_node.chainman)->GetConsensus()};
    const script_verify_flags base{SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_TAPROOT};
    const uint256 hash{uint256::ONE};
    BOOST_REQUIRE(!params.script_flag_exceptions.contains(hash));
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{}, params) == base);
    // The height in the coinbase is no script rule.
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.heightincb = true}, params) == base);
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.dersig = true}, params) == (base | SCRIPT_VERIFY_DERSIG));
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.cltv = true}, params) == (base | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY));
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.csv = true}, params) == (base | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY));
    BOOST_CHECK(GetBlockScriptFlags(hash, DeploymentFlags{.segwit = true}, params) == (base | SCRIPT_VERIFY_NULLDUMMY));

    BOOST_REQUIRE(!params.script_flag_exceptions.empty());
    for (const auto& [exception, flags] : params.script_flag_exceptions) {
        BOOST_CHECK(flags != base);
        BOOST_CHECK(GetBlockScriptFlags(exception, DeploymentFlags{}, params) == flags);
        BOOST_CHECK(GetBlockScriptFlags(exception, DeploymentFlags{.dersig = true}, params) == (flags | SCRIPT_VERIFY_DERSIG));
    }
}

BOOST_AUTO_TEST_SUITE_END()
