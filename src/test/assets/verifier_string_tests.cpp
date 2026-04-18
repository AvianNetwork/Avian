// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include <assets/LibBoolEE.h>
#include <assets/assets.h>
#include <test/util/setup_common.h>
#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(verifier_string_tests, BasicTestingSetup)

    BOOST_AUTO_TEST_CASE(booleanexpression_check)
    {
        BOOST_TEST_MESSAGE("Running Boolean Expression Test");

        std::set<std::string> setTrue;
        setTrue.insert("#TAG1");
        setTrue.insert("#TAG2");
        setTrue.insert("#TAG3");

        // Test simple cases
        BOOST_CHECK(BoolCalc().calculate("#TAG1", setTrue));
        BOOST_CHECK(!BoolCalc().calculate("#TAG4", setTrue));

        // Test AND
        BOOST_CHECK(BoolCalc().calculate("#TAG1&#TAG2", setTrue));
        BOOST_CHECK(!BoolCalc().calculate("#TAG1&#TAG4", setTrue));

        // Test OR
        BOOST_CHECK(BoolCalc().calculate("#TAG1|#TAG4", setTrue));
        BOOST_CHECK(!BoolCalc().calculate("#TAG5|#TAG4", setTrue));

        // Test Parenthesis
        BOOST_CHECK(BoolCalc().calculate("(#TAG1|#TAG4)&#TAG2", setTrue));
        BOOST_CHECK(!BoolCalc().calculate("(#TAG1|#TAG4)&#TAG5", setTrue));

        BOOST_CHECK(BoolCalc().calculate("(#TAG1&#TAG2)|(#TAG4&#TAG5)", setTrue));
        BOOST_CHECK(!BoolCalc().calculate("(#TAG1&#TAG4)|(#TAG5&#TAG6)", setTrue));

        // Test Complex
        BOOST_CHECK(BoolCalc().calculate("((#TAG1&#TAG2)|#TAG3)&#TAG1", setTrue));
        BOOST_CHECK(!BoolCalc().calculate("((#TAG1&#TAG4)|#TAG5)&#TAG2", setTrue));
    }

    BOOST_AUTO_TEST_CASE(verifier_string_check)
    {
        BOOST_TEST_MESSAGE("Running Verifier String Test");

        std::string error;

        // Test simple cases
        BOOST_CHECK(ContextualCheckVerifierString(nullptr, "#TAG1", "", error));
        BOOST_CHECK(!ContextualCheckVerifierString(nullptr, "TAG1", "", error)); // Missing #
        BOOST_CHECK(!ContextualCheckVerifierString(nullptr, "#tag1", "", error)); // lowercase
        BOOST_CHECK(!ContextualCheckVerifierString(nullptr, "", "", error)); // empty

        // Test AND
        BOOST_CHECK(ContextualCheckVerifierString(nullptr, "#TAG1&#TAG2", "", error));
        BOOST_CHECK(!ContextualCheckVerifierString(nullptr, "#TAG1&TAG2", "", error)); // Missing #

        // Test OR
        BOOST_CHECK(ContextualCheckVerifierString(nullptr, "#TAG1|#TAG2", "", error));

        // Test Parenthesis
        BOOST_CHECK(ContextualCheckVerifierString(nullptr, "(#TAG1|#TAG4)&#TAG2", "", error));

        // Test Complex
        BOOST_CHECK(ContextualCheckVerifierString(nullptr, "((#TAG1&#TAG2)|#TAG3)&#TAG1", "", error));
    }

    BOOST_AUTO_TEST_CASE(verifier_string_true_false_check)
    {
        BOOST_TEST_MESSAGE("Running Verifier String True False Test");

        std::string error;

        // Test simple cases
        BOOST_CHECK(ContextualCheckVerifierString(nullptr, "true", "", error));
        BOOST_CHECK(!ContextualCheckVerifierString(nullptr, "false", "", error));
        BOOST_CHECK(!ContextualCheckVerifierString(nullptr, "TRUE", "", error)); // uppercase
        BOOST_CHECK(!ContextualCheckVerifierString(nullptr, "FALSE", "", error));
    }


BOOST_AUTO_TEST_SUITE_END()
