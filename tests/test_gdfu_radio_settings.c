/* Host tests for persisted proxy radio settings. */
#include "gdfu_radio_settings.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                        \
  do                                                                       \
  {                                                                        \
    if (!(cond))                                                           \
    {                                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

static void test_missing_settings_use_defaults(void)
{
  const gdfu_radio_settings_t settings = gdfu_radio_settings_normalize(0u, 0u);
  CHECK(settings.profile == GDFU_PROFILE_ID_433);
  CHECK(settings.tx_power_dbm == 10u);
}

static void test_invalid_settings_use_defaults(void)
{
  const gdfu_radio_settings_t settings = gdfu_radio_settings_normalize(99u, 23u);
  CHECK(settings.profile == GDFU_PROFILE_ID_433);
  CHECK(settings.tx_power_dbm == 10u);
}

static void test_valid_settings_are_preserved(void)
{
  const gdfu_radio_settings_t settings = gdfu_radio_settings_normalize(GDFU_PROFILE_ID_868, 22u);
  CHECK(settings.profile == GDFU_PROFILE_ID_868);
  CHECK(settings.tx_power_dbm == 22u);
}

int main(void)
{
  test_missing_settings_use_defaults();
  test_invalid_settings_use_defaults();
  test_valid_settings_are_preserved();

  if (failures != 0)
  {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all proxy radio settings tests passed\n");
  return 0;
}
