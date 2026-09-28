#include "discovery.h"

// The discovery payloads have to fit PubSubClient's send buffer: anything
// larger is dropped without an error at the MQTT layer, and the entity simply
// never appears in Home Assistant. The recipe picker is the risk, because its
// payload carries one option per recipe on the card.
int main() {
  std::printf("\nfixed entities\n");
  publishHaDiscovery();
  std::printf("\nrecipe picker with a full card (%d recipes)\n", MAX_RECIPES);
  for (int i = 0; i < MAX_RECIPES; i++) {
    // The longest stem sanitizeRecipeStem() lets through.
    std::snprintf(recipes[i].stem, RECIPE_NAME_MAX_LEN, "recipe_%02d_aaaaaaaaaaaaaaaaaa", i);
  }
  recipeCount = MAX_RECIPES;
  publishHaRecipeSelect();

  std::printf("\nlargest frame %zu bytes, buffer %d bytes\n", maxFrame, MQTT_BUFFER_SIZE);
  if (overSized) {
    std::printf("FAIL: %d payload(s) exceed the buffer\n", overSized);
    return 1;
  }
  std::printf("all payloads fit\n");
  return 0;
}
