#include "reward.h"

static const char* messages[] = {
    "You did it!",
    "Well done!",
    "Great job!",
    "Congrats!",
    "Amazing work!",
    "Keep up the good work!",
    "Fantastic job!",
    "Bravo!",
    "Impressive!",
};

struct RewardWindow {
  Window* window;
  TextLayer* text;

  RewardCallbacks callbacks;
};

static void on_button(ClickRecognizerRef ref, void* ctx) {
  RewardWindow* reward = (RewardWindow*)ctx;

  (reward->callbacks.on_back)(reward->callbacks.userdata);
}

static void click_config_provider(void* ctx) {
  window_single_click_subscribe(BUTTON_ID_BACK, on_button);
  window_single_click_subscribe(BUTTON_ID_UP, on_button);
  window_single_click_subscribe(BUTTON_ID_SELECT, on_button);
  window_single_click_subscribe(BUTTON_ID_DOWN, on_button);
}

static void on_load(Window* window) {
  srand(time(NULL));
  RewardWindow* reward = (RewardWindow*)window_get_user_data(window);
  Layer* root = window_get_root_layer(window);
  GRect bounds = layer_get_unobstructed_bounds(root);

  window_set_click_config_provider_with_context(reward->window,
                                                click_config_provider, reward);
  char *message = (char *)messages[rand() % (sizeof(messages) / sizeof(messages[0]))];

  bounds.origin.y = bounds.size.h / 2 - graphics_text_layout_get_content_size(message, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD), bounds, GTextOverflowModeWordWrap, GTextAlignmentCenter).h / 2;
  reward->text = text_layer_create(bounds);
  text_layer_set_text(reward->text, message);
  text_layer_set_font(reward->text,
                      fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(reward->text, GTextAlignmentCenter);
  text_layer_set_overflow_mode(reward->text, GTextOverflowModeWordWrap);
  layer_add_child(root, text_layer_get_layer(reward->text));
}

static void on_unload(Window* window) {
  RewardWindow* reward = (RewardWindow*)window_get_user_data(window);

  text_layer_destroy(reward->text);
}

RewardWindow* reward_window_create(RewardCallbacks callbacks) {
  RewardWindow* reward = malloc(sizeof(RewardWindow));

  reward->callbacks = callbacks;

  reward->window = window_create();
  window_set_user_data(reward->window, reward);

  window_set_window_handlers(reward->window, (WindowHandlers){
                                                 .load = on_load,
                                                 .unload = on_unload,
                                             });

  return reward;
}

void reward_window_destroy(RewardWindow* reward) {
  window_destroy(reward->window);
  free(reward);
}

Window* reward_window_get_window(RewardWindow* reward) {
  return reward->window;
}
