#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TELE_MQTT_HOST_TEST 1
#include "tele_mqtt.h"

static int s_handler_calls;
static char s_handler_topic[128];
static char s_handler_payload[128];
static size_t s_handler_payload_len;

static esp_err_t shared_handler(const char *topic,
                                const char *payload,
                                size_t payload_len,
                                void *ctx)
{
    int *ctx_calls = (int *)ctx;

    s_handler_calls++;
    if (ctx_calls) {
        (*ctx_calls)++;
    }

    assert(topic != NULL);
    assert(payload != NULL);
    snprintf(s_handler_topic, sizeof(s_handler_topic), "%s", topic);
    assert(payload_len < sizeof(s_handler_payload));
    memcpy(s_handler_payload, payload, payload_len);
    s_handler_payload[payload_len] = '\0';
    s_handler_payload_len = payload_len;
    return ESP_OK;
}

static void reset_handler_capture(void)
{
    s_handler_calls = 0;
    s_handler_topic[0] = '\0';
    s_handler_payload[0] = '\0';
    s_handler_payload_len = 0;
}

static void test_topic_validation_and_construction(void)
{
    char topic[128] = {0};

    tele_mqtt_host_test_reset();
    tele_mqtt_host_test_set_base_topic("v1/test");

    assert(tele_mqtt_host_test_build_shared_topic("example/event",
                                                  topic,
                                                  sizeof(topic)) == ESP_OK);
    assert(strcmp(topic, "v1/test/_shared/example/event") == 0);

    assert(tele_mqtt_host_test_build_shared_topic("", topic, sizeof(topic)) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_host_test_build_shared_topic("/example", topic, sizeof(topic)) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_host_test_build_shared_topic("example/", topic, sizeof(topic)) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_host_test_build_shared_topic("example//event", topic, sizeof(topic)) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_host_test_build_shared_topic("example/+", topic, sizeof(topic)) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_host_test_build_shared_topic("example/#", topic, sizeof(topic)) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_host_test_build_shared_topic("example/event", topic, 8) == ESP_ERR_INVALID_ARG);
}

static void test_subscription_registry(void)
{
    int ctx_calls = 0;

    tele_mqtt_host_test_reset();
    tele_mqtt_host_test_set_base_topic("v1/test");

    assert(tele_mqtt_subscribe_shared("example/event", 1, shared_handler, &ctx_calls) == ESP_OK);
    assert(tele_mqtt_subscribe_shared("example/event", 1, shared_handler, &ctx_calls) == ESP_ERR_INVALID_STATE);
    assert(tele_mqtt_subscribe_shared("example/state", 0, shared_handler, &ctx_calls) == ESP_OK);
    assert(tele_mqtt_subscribe_shared("example/bad", 0, NULL, NULL) == ESP_ERR_INVALID_ARG);

    for (size_t i = tele_mqtt_host_test_shared_subscription_count();
         i < TELE_MQTT_HOST_TEST_MAX_SHARED_SUBSCRIPTIONS;
         ++i) {
        char suffix[32] = {0};
        snprintf(suffix, sizeof(suffix), "bulk/%u", (unsigned)i);
        assert(tele_mqtt_subscribe_shared(suffix, 0, shared_handler, &ctx_calls) == ESP_OK);
    }
    assert(tele_mqtt_subscribe_shared("bulk/overflow", 0, shared_handler, NULL) == ESP_ERR_NO_MEM);
}

static void test_immediate_subscribe_and_reconnect_resubscribe(void)
{
    int ctx_calls = 0;

    tele_mqtt_host_test_reset();
    tele_mqtt_host_test_set_base_topic("v1/test");
    tele_mqtt_host_test_set_connected(true);

    assert(tele_mqtt_subscribe_shared("example/event", 1, shared_handler, &ctx_calls) == ESP_OK);
    assert(tele_mqtt_host_test_subscribe_count() == 1);
    assert(strcmp(tele_mqtt_host_test_last_subscribed_topic(), "v1/test/_shared/example/event") == 0);
    assert(tele_mqtt_host_test_last_subscribed_qos() == 1);

    tele_mqtt_host_test_set_connected(false);
    tele_mqtt_host_test_set_connected(true);
    assert(tele_mqtt_host_test_subscribe_count() == 2);
    assert(strcmp(tele_mqtt_host_test_last_subscribed_topic(), "v1/test/_shared/example/event") == 0);
}

static void test_early_registration_refreshes_topic_when_base_topic_changes(void)
{
    int ctx_calls = 0;

    tele_mqtt_host_test_reset();

    assert(tele_mqtt_subscribe_shared("example/event", 1, shared_handler, &ctx_calls) == ESP_OK);
    tele_mqtt_host_test_set_base_topic("v1/product");
    tele_mqtt_host_test_set_connected(true);

    assert(tele_mqtt_host_test_subscribe_count() == 1);
    assert(strcmp(tele_mqtt_host_test_last_subscribed_topic(), "v1/product/_shared/example/event") == 0);
}

static void test_publish_shared(void)
{
    tele_mqtt_host_test_reset();
    tele_mqtt_host_test_set_base_topic("v1/test");

    assert(tele_mqtt_publish_shared("example/event", "{}", 1, false) == ESP_ERR_INVALID_STATE);

    tele_mqtt_host_test_set_connected(true);
    assert(tele_mqtt_publish_shared("example/event", "{\"ok\":true}", 1, false) == ESP_OK);
    assert(strcmp(tele_mqtt_host_test_last_published_topic(), "v1/test/_shared/example/event") == 0);
    assert(strcmp(tele_mqtt_host_test_last_published_payload(), "{\"ok\":true}") == 0);
    assert(tele_mqtt_host_test_last_published_qos() == 1);
    assert(!tele_mqtt_host_test_last_published_retain());

    assert(tele_mqtt_publish_shared("example/#", "{}", 1, false) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_publish_shared("example/event", "{}", -1, false) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_publish_shared("example/event", "{}", 3, false) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_publish_shared("example/empty", "", 0, true) == ESP_OK);
    assert(tele_mqtt_host_test_last_published_retain());
}

static void test_get_device_id_contract(void)
{
    char out[64] = {0};

    tele_mqtt_host_test_reset();
    assert(tele_mqtt_get_device_id(NULL, sizeof(out)) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_get_device_id(out, 0) == ESP_ERR_INVALID_ARG);
    assert(tele_mqtt_get_device_id(out, sizeof(out)) == ESP_ERR_INVALID_STATE);

    tele_mqtt_host_test_set_device_id("TCafe-A1B2C3");
    assert(tele_mqtt_get_device_id(out, sizeof(out)) == ESP_OK);
    assert(strcmp(out, "TCafe-A1B2C3") == 0);
    assert(tele_mqtt_get_device_id(out, 4) == ESP_ERR_INVALID_SIZE);
}

static void test_inbound_dispatch(void)
{
    int ctx_calls = 0;

    tele_mqtt_host_test_reset();
    tele_mqtt_host_test_set_base_topic("v1/test");
    assert(tele_mqtt_subscribe_shared("example/event", 1, shared_handler, &ctx_calls) == ESP_OK);

    reset_handler_capture();
    assert(tele_mqtt_host_test_dispatch_data("v1/test/_shared/other", "no", 2) == false);
    assert(s_handler_calls == 0);

    assert(tele_mqtt_host_test_dispatch_data("v1/test/_shared/example/event", "payload", 7) == true);
    assert(s_handler_calls == 1);
    assert(ctx_calls == 1);
    assert(strcmp(s_handler_topic, "v1/test/_shared/example/event") == 0);
    assert(strcmp(s_handler_payload, "payload") == 0);
    assert(s_handler_payload_len == 7);

    assert(tele_mqtt_host_test_dispatch_data("v1/test/device/cmd/in", "{}", 2) == false);
    assert(s_handler_calls == 1);
}

static void test_subscription_registry_is_locked_for_mutation_and_dispatch(void)
{
    int ctx_calls = 0;

    tele_mqtt_host_test_reset();
    tele_mqtt_host_test_set_base_topic("v1/test");

    assert(tele_mqtt_subscribe_shared("example/event", 1, shared_handler, &ctx_calls) == ESP_OK);
    assert(tele_mqtt_host_test_lock_take_count() > 0);
    assert(tele_mqtt_host_test_lock_take_count() == tele_mqtt_host_test_lock_give_count());

    int lock_count_after_subscribe = tele_mqtt_host_test_lock_take_count();

    assert(tele_mqtt_host_test_dispatch_data("v1/test/_shared/example/event", "payload", 7) == true);
    assert(tele_mqtt_host_test_lock_take_count() > lock_count_after_subscribe);
    assert(tele_mqtt_host_test_lock_take_count() == tele_mqtt_host_test_lock_give_count());
}

int main(void)
{
    test_topic_validation_and_construction();
    test_subscription_registry();
    test_immediate_subscribe_and_reconnect_resubscribe();
    test_early_registration_refreshes_topic_when_base_topic_changes();
    test_publish_shared();
    test_get_device_id_contract();
    test_inbound_dispatch();
    test_subscription_registry_is_locked_for_mutation_and_dispatch();
    return 0;
}
