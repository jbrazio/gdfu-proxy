/* Host test for the proxy session/discovery engine. Assert-based, no framework. */
#include "gdfu_proxy_state.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                        \
  do                                                                       \
  {                                                                        \
    if (!(cond))                                                           \
    {                                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

static void test_beacon_ttl_and_capacity(void)
{
  gdfu_proxy_state_t s;
  gdfu_proxy_init(&s);

  gdfu_proxy_on_beacon(&s, 0x01020304u, 1, -60, 1000u);
  gdfu_proxy_on_beacon(&s, 0x05060708u, 2, -70, 2000u);
  CHECK(gdfu_proxy_live_beacons(&s, 6000u) == 2);
  CHECK(gdfu_proxy_live_beacons(&s, 16500u) == 1); /* first beacon expired at 1000+15000 */

  for (uint32_t i = 1; i <= GDFU_PROXY_DISCOVERY_CAPACITY; ++i)
  {
    gdfu_proxy_on_beacon(&s, 0x1000u + i, 1, -50, 7000u + i);
  }
  CHECK(s.beacon_count == GDFU_PROXY_DISCOVERY_CAPACITY);
  /* inserting one more when full evicts the oldest (target 0x1001) */
  gdfu_proxy_on_beacon(&s, 0x2000u, 1, -50, 7100u);
  CHECK(s.beacon_count == GDFU_PROXY_DISCOVERY_CAPACITY);
  bool oldest_gone = true;
  for (size_t i = 0; i < s.beacon_count; ++i)
  {
    if (s.beacons[i].target_id == 0x1001u) oldest_gone = false;
  }
  CHECK(oldest_gone);
}

static void test_select_requires_live_beacon(void)
{
  gdfu_proxy_state_t s;
  gdfu_proxy_init(&s);
  gdfu_proxy_on_beacon(&s, 0xAABBCCDDu, 1, -55, 100u);
  CHECK(gdfu_proxy_select_target(&s, 0xAABBCCDDu, 300u));
  CHECK(s.phase == GDFU_PROXY_SELECTED);
  CHECK(!gdfu_proxy_select_target(&s, 0xDEADBEEFu, 400u));
  /* beacon older than TTL cannot be selected */
  gdfu_proxy_state_t t;
  gdfu_proxy_init(&t);
  gdfu_proxy_on_beacon(&t, 0x11223344u, 1, -55, 100u);
  CHECK(!gdfu_proxy_select_target(&t, 0x11223344u, 100u + GDFU_PROXY_DISCOVERY_TTL_MS + 1u));
}

static void test_start_session_rules(void)
{
  gdfu_proxy_state_t s;
  gdfu_proxy_init(&s);
  CHECK(!gdfu_proxy_start_session(&s, 42u, 10u)); /* no selection */
  gdfu_proxy_on_beacon(&s, 0xAABBCCDDu, 1, -55, 10u);
  CHECK(gdfu_proxy_select_target(&s, 0xAABBCCDDu, 20u));
  CHECK(!gdfu_proxy_start_session(&s, 0u, 30u)); /* zero session invalid */
  CHECK(gdfu_proxy_start_session(&s, 42u, 30u));
  CHECK(s.phase == GDFU_PROXY_STREAMING);
  CHECK(s.next_sequence == 0u);
  CHECK(!gdfu_proxy_start_session(&s, 43u, 40u)); /* already streaming */
}

static void test_ack_advances_and_retries(void)
{
  gdfu_proxy_state_t s;
  gdfu_proxy_init(&s);
  gdfu_proxy_on_beacon(&s, 1u, 1, -50, 0u);
  gdfu_proxy_select_target(&s, 1u, 0u);
  gdfu_proxy_start_session(&s, 7u, 0u);
  s.next_sequence = 3u;
  s.confirmed_offset = 96u;

  /* stale sequence is ignored */
  CHECK(!gdfu_proxy_on_ack(&s, 7u, 2u, 96u, 10u));
  /* wrong session id is ignored */
  CHECK(!gdfu_proxy_on_ack(&s, 99u, 3u, 192u, 10u));
  CHECK(gdfu_proxy_on_ack(&s, 7u, 3u, 192u, 10u));
  CHECK(s.confirmed_offset == 192u);
  CHECK(s.next_sequence == 4u);
  CHECK(s.retries == 0u);

  gdfu_proxy_note_retx(&s);
  CHECK(s.retries == 1u);
  CHECK(gdfu_proxy_can_retry(&s));
  for (int i = 0; i < 10; ++i) gdfu_proxy_note_retx(&s);
  CHECK(!gdfu_proxy_can_retry(&s));
}

static void test_tx_power_range(void)
{
  CHECK(!gdfu_proxy_tx_power_valid(9u));
  CHECK(gdfu_proxy_tx_power_valid(10u));
  CHECK(gdfu_proxy_tx_power_valid(22u));
  CHECK(!gdfu_proxy_tx_power_valid(23u));
}

static void test_nack_is_terminal_protocol_result(void)
{
  gdfu_proxy_state_t s;
  gdfu_proxy_init(&s);
  gdfu_proxy_on_beacon(&s, 1u, 1, -50, 0u);
  gdfu_proxy_select_target(&s, 1u, 0u);
  gdfu_proxy_start_session(&s, 7u, 0u);

  gdfu_proxy_on_nack(&s, 9u);
  CHECK(s.last_error == 9u);
  CHECK(s.retries == GDFU_MAX_RETRIES + 1u);
  CHECK(!gdfu_proxy_can_retry(&s));
}

static void test_session_expiry_and_abort(void)
{
  gdfu_proxy_state_t s;
  gdfu_proxy_init(&s);
  gdfu_proxy_on_beacon(&s, 1u, 1, -50, 0u);
  gdfu_proxy_select_target(&s, 1u, 0u);
  gdfu_proxy_start_session(&s, 9u, 100u);
  gdfu_proxy_on_peer_traffic(&s, 20000u);
  CHECK(!gdfu_proxy_session_expired(&s, 40000u));
  CHECK(gdfu_proxy_session_expired(&s, 20000u + GDFU_PROXY_SESSION_TIMEOUT_MS + 1u));

  gdfu_proxy_abort(&s);
  CHECK(s.phase == GDFU_PROXY_IDLE);
  CHECK(s.session_id == 0u);
  CHECK(s.selected_target == 0u);
  CHECK(s.next_sequence == 0u);
  CHECK(s.confirmed_offset == 0u);
}

static void test_two_proxies_independent_selection(void)
{
  /* Proxies A and B in the same vicinity are two independent state machines
   * over the same beacon stream: each must select its own target and reject
   * the other proxy's traffic (session isolation without any shared state). */
  gdfu_proxy_state_t a;
  gdfu_proxy_state_t b;
  gdfu_proxy_init(&a);
  gdfu_proxy_init(&b);

  /* Same two beacons reach both proxies. */
  gdfu_proxy_on_beacon(&a, 0xAAAA0001u, 1, -60, 100u);
  gdfu_proxy_on_beacon(&a, 0xAAAA0002u, 1, -70, 110u);
  gdfu_proxy_on_beacon(&b, 0xAAAA0001u, 1, -65, 100u);
  gdfu_proxy_on_beacon(&b, 0xAAAA0002u, 1, -75, 110u);

  /* A selects target 1, B selects target 2 — independent. */
  CHECK(gdfu_proxy_select_target(&a, 0xAAAA0001u, 200u));
  CHECK(gdfu_proxy_select_target(&b, 0xAAAA0002u, 210u));
  CHECK(a.selected_target == 0xAAAA0001u);
  CHECK(b.selected_target == 0xAAAA0002u);
  CHECK(a.phase == GDFU_PROXY_SELECTED);
  CHECK(b.phase == GDFU_PROXY_SELECTED);

  /* Sessions start; ACKs addressed to the other session must be rejected. */
  CHECK(gdfu_proxy_start_session(&a, 0x1111u, 250u));
  CHECK(gdfu_proxy_start_session(&b, 0x2222u, 260u));
  a.next_sequence = 0u;
  b.next_sequence = 0u;

  /* A receives B's ACK (wrong session): ignored, state unchanged. */
  CHECK(!gdfu_proxy_on_ack(&a, 0x2222u, 0u, 192u, 300u)); /* B's session */
  CHECK(a.confirmed_offset == 0u);
  CHECK(a.next_sequence == 0u);
  /* B receives A's ACK (wrong session): ignored, state unchanged. */
  CHECK(!gdfu_proxy_on_ack(&b, 0x1111u, 0u, 192u, 310u)); /* A's session */
  CHECK(b.confirmed_offset == 0u);

  /* Correct ACKs advance only their own machine. */
  CHECK(gdfu_proxy_on_ack(&a, 0x1111u, 0u, 192u, 320u));
  CHECK(a.confirmed_offset == 192u);
  CHECK(a.next_sequence == 1u);
  CHECK(b.confirmed_offset == 0u);

  /* A aborts; B's active session is untouched. */
  gdfu_proxy_abort(&a);
  CHECK(a.phase == GDFU_PROXY_IDLE);
  CHECK(b.phase == GDFU_PROXY_STREAMING);
  CHECK(b.session_id == 0x2222u);
}

int main(void)
{
  test_beacon_ttl_and_capacity();
  test_select_requires_live_beacon();
  test_start_session_rules();
  test_ack_advances_and_retries();
  test_nack_is_terminal_protocol_result();
  test_tx_power_range();
  test_session_expiry_and_abort();
  test_two_proxies_independent_selection();

  if (failures != 0)
  {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all proxy state tests passed\n");
  return 0;
}
