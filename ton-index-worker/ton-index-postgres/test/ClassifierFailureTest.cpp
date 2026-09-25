#include "ClassifierFailure.h"

#include <exception>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char *description) {
  if (!ok) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

template <class Exception>
std::exception_ptr capture(Exception exception) {
  try {
    throw exception;
  } catch (...) {
    return std::current_exception();
  }
}

template <class Expected>
void check_propagates(std::exception_ptr error, const char *description) {
  try {
    mch::classifier_failure_reason(error, "sql: ", "exception: ");
  } catch (const Expected &) {
    return;
  } catch (...) {
    check(false, description);
    return;
  }
  check(false, description);
}

}  // namespace

int main() {
  const auto rejected = capture(pqxx::unique_violation{"duplicate key", "INSERT", "23505"});
  check(mch::classifier_failure_reason(rejected, "sql: ", "exception: ") ==
            "sql: 23505 duplicate key",
        "23505 yields a reason");
  check(mch::classifier_failure_reason(
            capture(pqxx::data_exception{"division by zero", "INSERT", "22012"}),
            "sql: ", "exception: ") == "sql: 22012 division by zero",
        "22012 yields a reason");
  check(mch::classifier_failure_reason(
            capture(pqxx::sql_error{"program limit exceeded", "INSERT", "54001"}),
            "sql: ", "exception: ") == "sql: 54001 program limit exceeded",
        "54001 yields a reason");

  check_propagates<pqxx::serialization_failure>(
      capture(pqxx::serialization_failure{"serialization failure", "INSERT", "40001"}),
      "40001 propagates");
  check_propagates<pqxx::undefined_table>(
      capture(pqxx::undefined_table{"undefined table", "INSERT", "42P01"}),
      "42P01 propagates");
  check_propagates<pqxx::broken_connection>(capture(pqxx::broken_connection{"lost connection"}),
                                             "broken_connection propagates");
  check_propagates<pqxx::in_doubt_error>(capture(pqxx::in_doubt_error{"unknown outcome"}),
                                          "in_doubt_error propagates");
  check_propagates<std::bad_alloc>(capture(std::bad_alloc{}), "bad_alloc propagates");
  check(mch::classifier_failure_reason(capture(std::runtime_error{"bad json"}),
                                       "sql: ", "exception: ") == "exception: bad json",
        "std::exception yields a reason");

  if (failures != 0) return 1;
  std::cout << "classifier_failure: OK\n";
  return 0;
}
