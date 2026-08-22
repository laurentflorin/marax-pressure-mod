#include "discovery.h"

// The discovery payloads have to fit PubSubClient's send buffer: anything
// larger is dropped without an error at the MQTT layer, and the entity simply
// never appears in Home Assistant. The profile picker is the risk, because its
// payload carries one option per profile on the card.
int main() {
  std::printf("\nfixed entities\n");
  publishHaDiscovery();
  std::printf("\nprofile picker with a full card (%d profiles)\n", MAX_PROFILES);
  for (int i = 0; i < MAX_PROFILES; i++) {
    // Names as long as the buffer allows, so this is the true worst case.
    std::snprintf(profileNames[i], PROFILE_NAME_MAX_LEN,
                  "profile_%02d_aaaaaaaaaaaaaaaa.csv", i);
  }
  profileCount = MAX_PROFILES;
  publishHaProfileSelect();

  std::printf("\nlargest frame %zu bytes, buffer %d bytes\n", maxFrame, MQTT_BUFFER_SIZE);
  if (overSized) {
    std::printf("FAIL: %d payload(s) exceed the buffer\n", overSized);
    return 1;
  }
  std::printf("all payloads fit\n");
  return 0;
}
