#pragma once
#include <cstdint>
#include <ctime>
#include <gromox/mapi_types.hpp>

extern uint16_t rop_util_get_replid(eid_t);
extern uint64_t rop_util_get_gc_value(eid_t);
extern GLOBCNT rop_util_get_gc_array(eid_t);
extern GLOBCNT rop_util_value_to_gc(uint64_t);
extern uint64_t rop_util_gc_to_value(GLOBCNT);
extern eid_t rop_util_make_eid(uint16_t replid, GLOBCNT);
extern eid_t rop_util_make_eid_ex(uint16_t replid, uint64_t value);
GUID rop_util_make_user_guid(int user_id);
GUID rop_util_make_domain_guid(int domain_id);
extern GX_EXPORT int rop_util_get_user_id(GUID);
extern GX_EXPORT int rop_util_get_domain_id(GUID);
uint64_t rop_util_unix_to_nttime(time_t unix_time);
time_t rop_util_nttime_to_unix(uint64_t nt_time);
extern uint64_t rop_util_current_nttime();
GUID rop_util_binary_to_guid(const BINARY *pbin);
void rop_util_guid_to_binary(GUID guid, BINARY *pbin);
void rop_util_free_binary(BINARY *pbin);
namespace gromox {
extern GX_EXPORT uint64_t apptime_to_nttime_approx(double);
extern GX_EXPORT uint32_t props_to_defer_interval(const TPROPVAL_ARRAY &);
}
