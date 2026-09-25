#pragma once

#include <pqxx/pqxx>

#include <exception>
#include <new>
#include <string>
#include <string_view>

namespace mch {

inline bool insert_rejected(const std::string &sqlstate) {
  return sqlstate.starts_with("22") || sqlstate.starts_with("23") || sqlstate.starts_with("54");
}

inline std::string bounded_reason(std::string reason) {
  return reason.substr(0, reason.find_first_of("\r\n")).substr(0, 200);
}

inline std::string classifier_failure_reason(std::exception_ptr error,
                                             std::string_view sql_prefix,
                                             std::string_view exception_prefix) {
  try {
    std::rethrow_exception(error);
  } catch (const pqxx::broken_connection &) {
    throw;
  } catch (const pqxx::in_doubt_error &) {
    throw;
  } catch (const std::bad_alloc &) {
    throw;
  } catch (const pqxx::sql_error &e) {
    if (!insert_rejected(e.sqlstate())) throw;
    return bounded_reason(std::string(sql_prefix) + e.sqlstate() + " " + e.what());
  } catch (const std::exception &e) {
    return bounded_reason(std::string(exception_prefix) + e.what());
  } catch (...) {
    return bounded_reason(std::string(exception_prefix) + "unknown exception");
  }
}

}  // namespace mch
