#pragma once
#include "../../shared.hpp"

#include <memory>
#include <vector>

inline std::vector<std::shared_ptr<CTestCase>> pluginsTestCases;

#ifndef INCLUDED_FROM_MAIN
// What this group of tests is called
#define TEST_GROUP_NAME "plugins"
// Where our group's test cases will be stored
#define GROUP_TEST_CASE_STORAGE pluginsTestCases
#endif // !defined(INCLUDED_FROM_MAIN)
