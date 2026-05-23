#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#define MOSFET_GPIO GPIO_NUM_2
#define BUZZER_GPIO GPIO_NUM_21

// XIAO ESP32-C6 FM8625H RF switch: GPIO3 powers the switch (active LOW),
// GPIO14 selects the antenna (LOW = on-board ceramic, HIGH = external U.FL).
#define ANT_RF_SWITCH_GPIO CONFIG_ANT_RF_SWITCH_GPIO
#define ANT_SELECT_GPIO CONFIG_ANT_SELECT_GPIO
#define FIRESTARTER_WIFI_SSID CONFIG_FIRESTARTER_WIFI_SSID
#define FIRESTARTER_WIFI_PASSWORD CONFIG_FIRESTARTER_WIFI_PASSWORD

static const char *TAG = "firestarter";
static httpd_handle_t server_handle = NULL;
static TaskHandle_t launch_task_handle = NULL;
static volatile bool launch_in_progress = false;
static volatile bool launch_abort = false;
static volatile bool launch_committed = false;   // true после точки фиксации — отмена уже невозможна
static volatile uint32_t launch_epoch = 0;        // ID текущего запуска; адресная отмена через /cancel?id=
static portMUX_TYPE launch_mux = portMUX_INITIALIZER_UNLOCKED;

// Встроенный HTML файл
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

// ================= WebSocket =================
#ifdef CONFIG_HTTPD_WS_SUPPORT
#define WS_MAX_CLIENTS 4
static int ws_clients[WS_MAX_CLIENTS] = { -1, -1, -1, -1 };
// Размер буфера под httpd_get_client_list(): обязан быть >= max_open_sockets
// (см. start_webserver, там 6). Берём с запасом.
#define WS_FDS_MAX 8

static void ws_add_client(int fd) {
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        if (ws_clients[i] == -1) { ws_clients[i] = fd; return; }
        if (ws_clients[i] == fd) return;
    }
    ESP_LOGW(TAG, "Список WS клиентов переполнен");
}

static void ws_remove_client(int fd) {
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) if (ws_clients[i] == fd) ws_clients[i] = -1;
}

static void ws_broadcast_json(const char *json) {
    if (!server_handle || !json) return;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)json,
        .len = strlen(json)
    };
    // Берём список клиентов у самого сервера, а не из ручного ws_clients[].
    // Ручная регистрация в ветке HTTP_GET обработчика /ws по факту не наполняла
    // массив (на устройстве отсчёт шёл — пищалка работала, — но кадры WS уходили
    // в пустой список, поэтому «Отмена»/цифры в браузер не приходили).
    // httpd_get_client_list() отдаёт реальные сокеты сессий, httpd_ws_get_fd_info()
    // отбирает из них WebSocket-клиентов, а httpd_ws_send_data() корректно шлёт кадр
    // из любой задачи (внутри ставит работу в очередь httpd и блокирует до отправки).
    size_t num_fds = WS_FDS_MAX;
    int client_fds[WS_FDS_MAX];
    if (httpd_get_client_list(server_handle, &num_fds, client_fds) != ESP_OK) return;
    for (size_t i = 0; i < num_fds; ++i) {
        if (httpd_ws_get_fd_info(server_handle, client_fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
        esp_err_t err = httpd_ws_send_data(server_handle, client_fds[i], &frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WS send -> %d, ошибка %s", client_fds[i], esp_err_to_name(err));
        }
    }
}

// Отправка одного JSON-кадра конкретному клиенту (для снимка состояния при подключении).
static void ws_send_to_fd(int fd, const char *json) {
    if (!server_handle || !json) return;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)json,
        .len = strlen(json)
    };
    httpd_ws_send_frame_async(server_handle, fd, &frame);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        const int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        ESP_LOGI(TAG, "WS подключен: fd=%d", fd);
        // Снимок состояния: клиент, подключившийся во время запуска, должен сразу
        // увидеть «Отмена» (или 🚀, если пуск уже зафиксирован), а не пустой «Запуск».
        if (launch_in_progress) {
            if (launch_committed) {
                ws_send_to_fd(fd, "{\"type\":\"launch\",\"message\":\"ЗАПУСК!\"}");
            } else {
                char snap[80];
                snprintf(snap, sizeof(snap),
                         "{\"type\":\"countdown_start\",\"count\":9,\"id\":%lu}",
                         (unsigned long)launch_epoch);
                ws_send_to_fd(fd, snap);
            }
        }
        return ESP_OK;
    }

    httpd_ws_frame_t recv_pkt = { 0 };
    esp_err_t ret = httpd_ws_recv_frame(req, &recv_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws_recv len err: %s", esp_err_to_name(ret));
        ws_remove_client(httpd_req_to_sockfd(req));
        return ret;
    }
    if (recv_pkt.len) {
        uint8_t *buf = (uint8_t *)calloc(1, recv_pkt.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        recv_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &recv_pkt, recv_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ws_recv data err: %s", esp_err_to_name(ret));
            free(buf);
            ws_remove_client(httpd_req_to_sockfd(req));
            return ret;
        }
        ESP_LOGI(TAG, "WS msg type=%d, len=%d: %s", recv_pkt.type, (int)recv_pkt.len, (char*)recv_pkt.payload);
        if (recv_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            ws_remove_client(httpd_req_to_sockfd(req));
        }
        free(buf);
    }
    return ESP_OK;
}
#else
// Fallback no-op when WebSocket is not enabled in HTTPD
static void ws_broadcast_json(const char *json) { (void)json; }
#endif

// Совместимый интерфейс отправки статусов (JSON)
static inline void send_ws_message(const char *message) {
    ws_broadcast_json(message);
}

// ================= Логика звука и запуска =================
// Ждёт ms миллисекунд срезами по 50 мс, чтобы отсчёт можно было прервать
// в середине ожидания. Возвращает true, если запросили отмену запуска.
static bool cancellable_delay_ms(int ms) {
    const int slice_ms = 50;
    int waited = 0;
    while (waited < ms) {
        if (launch_abort) return true;
        int chunk = (ms - waited) < slice_ms ? (ms - waited) : slice_ms;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        waited += chunk;
    }
    return launch_abort;
}

// Возвращает true, если тон был прерван отменой (зуммер при этом глушится).
bool beep(const int freq, const int duration_ms) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 128);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    bool aborted = cancellable_delay_ms(duration_ms);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    return aborted;
}

// Непрерываемый тон: для подтверждающего сигнала после точки фиксации,
// который нельзя глушить отменой (предупреждение о неизбежном пуске).
static void beep_blocking(const int freq, const int duration_ms) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 128);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Возвращает true, если запуск был отменён во время счёта. Окно отмены —
// только 9 шагов счёта; после сообщения "launch" запуск зафиксирован.
bool countdown_beeps() {
    const int countDown = 9;
    int currentCount = countDown;
    char msg[80];

    snprintf(msg, sizeof(msg),
             "{\"type\":\"countdown_start\",\"count\":9,\"id\":%lu}", (unsigned long)launch_epoch);
    send_ws_message(msg);
    if (cancellable_delay_ms(1000)) return true;

    for (int i = 1; i <= countDown; i++) {
        if (launch_abort) return true;

        snprintf(msg, sizeof(msg),
                 "{\"type\":\"countdown\",\"count\":%d,\"id\":%lu}", currentCount, (unsigned long)launch_epoch);
        send_ws_message(msg);

        if (beep(300 + i * 100, 600)) return true;
        currentCount--;
        ESP_LOGI(TAG, "%d", currentCount);
        if (cancellable_delay_ms(1000)) return true;
    }

    // Точка фиксации: дальше отмена не действует. Помечаем ДО рассылки "launch",
    // чтобы поздно подключившийся клиент получил корректный снимок.
    launch_committed = true;
    send_ws_message("{\"type\":\"launch\",\"message\":\"ЗАПУСК!\"}");
    beep_blocking(1500, 2000); // подтверждающий тон нельзя глушить отменой
    return false;
}

void launch_task(void *pvParameters) {
    // launch_in_progress уже выставлен в launch_handler (до создания задачи),
    // чтобы параллельный /launch или /cancel сразу видел активный запуск.
    ESP_LOGI(TAG, "Начинаем обратный отсчет");

    if (countdown_beeps()) {
        // Отмена: до MOSFET дело не дошло.
        ESP_LOGI(TAG, "Запуск отменён");
        send_ws_message("{\"type\":\"cancelled\",\"message\":\"Запуск отменён\"}");
        launch_abort = false;
        launch_in_progress = false;
        launch_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Запуск!");
    gpio_set_level(MOSFET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    gpio_set_level(MOSFET_GPIO, 0);
    ESP_LOGI(TAG, "Пуск выполнен");

    send_ws_message("{\"type\":\"status\",\"message\":\"🚀 Пуск выполнен\"}");

    launch_abort = false; // отмена в момент фиксации не действует — гасим флаг, чтобы вне запуска он был false
    launch_committed = false;
    launch_in_progress = false;
    launch_task_handle = NULL;
    vTaskDelete(NULL);
}

void startup_melody() {
    ESP_LOGI(TAG, "Система готова к работе");
    beep(523, 200);
    beep(659, 200);
    beep(784, 200);
    beep(1047, 400);
    vTaskDelay(pdMS_TO_TICKS(300));
    beep(1047, 800);
    vTaskDelay(pdMS_TO_TICKS(1000));
    send_ws_message("{\"type\":\"status\",\"message\":\"Система готова к работе\"}");
}

esp_err_t root_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Запрос главной страницы");
    httpd_resp_send(req, (const char*)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

esp_err_t launch_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Запрос запуска");

    // Занимаем слот запуска атомарно ДО создания задачи. Раньше launch_in_progress
    // выставлялся внутри задачи — два быстрых /launch успевали пройти проверку,
    // пока задача ещё не стартовала, и порождали два пуска (двойное срабатывание MOSFET).
    portENTER_CRITICAL(&launch_mux);
    if (launch_in_progress) {
        portEXIT_CRITICAL(&launch_mux);
        httpd_resp_sendstr(req, "Запуск уже выполняется");
        return ESP_OK;
    }
    launch_in_progress = true;
    launch_committed = false;
    launch_abort = false;
    launch_epoch++;            // новый ID запуска — для адресной отмены
    portEXIT_CRITICAL(&launch_mux);

    if (xTaskCreate(launch_task, "launch_task", 4096, NULL, 5, &launch_task_handle) == pdPASS) {
        httpd_resp_sendstr(req, "Запуск инициирован");
    } else {
        // Откат: задача не создана — освобождаем слот.
        launch_in_progress = false;
        launch_task_handle = NULL;
        httpd_resp_sendstr(req, "Ошибка запуска");
    }
    return ESP_OK;
}

esp_err_t cancel_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Запрос отмены");

    // Необязательный ?id=N — отменяем только конкретный запуск, чтобы запоздавший
    // в сети /cancel от прошлого пуска не прервал уже следующий.
    uint32_t req_epoch = 0;
    bool have_epoch = false;
    char query[40];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "id", val, sizeof(val)) == ESP_OK) {
            req_epoch = (uint32_t)strtoul(val, NULL, 10);
            have_epoch = true;
        }
    }

    bool accepted = false;
    portENTER_CRITICAL(&launch_mux);
    // Отмена возможна только до точки фиксации и только для текущего запуска.
    if (launch_in_progress && !launch_committed && (!have_epoch || req_epoch == launch_epoch)) {
        launch_abort = true;
        accepted = true;
    }
    portEXIT_CRITICAL(&launch_mux);

    httpd_resp_sendstr(req, accepted ? "Отмена запущена" : "Нет активного запуска");
    return ESP_OK;
}

// ===== Антенна (XIAO ESP32-C6, RF-переключатель FM8625H) =====
typedef enum { ANT_INT_ANT0 = 0, ANT_EXT_ANT1 = 1, ANT_AUTO = 2 } ant_mode_t;
static ant_mode_t g_current_ant = ANT_EXT_ANT1; // внешняя по умолчанию

static void ant_gpio_apply(ant_mode_t ant) {
    // RF-переключатель должен быть запитан (GPIO3 = LOW), иначе антенна
    // подключена только за счёт утечки сигнала. GPIO14 выбирает антенну:
    // HIGH = внешняя (U.FL), LOW = встроенная керамическая.
    gpio_set_level(ANT_RF_SWITCH_GPIO, 0);
    gpio_set_level(ANT_SELECT_GPIO, (ant == ANT_EXT_ANT1) ? 1 : 0);
}

static esp_err_t ant_set(ant_mode_t ant) {
    g_current_ant = ant;
    ant_gpio_apply(ant); // Ensure GPIOs are configured correctly
    ESP_LOGI(TAG, "Antenna set to %s", ant == ANT_INT_ANT0 ? "ANT0" : (ant == ANT_EXT_ANT1 ? "ANT1" : "AUTO"));
    return ESP_OK;
}

static esp_err_t ant_status_get(ant_mode_t *out) {
    if (out) *out = g_current_ant;
    return ESP_OK;
}

static esp_err_t http_ant_set_handler(httpd_req_t *req, ant_mode_t ant) {
    esp_err_t err = ant_set(ant);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"ant\":%d}", err == ESP_OK ? "true" : "false", (int)ant);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t http_internal_antenna_handler(httpd_req_t *req) { return http_ant_set_handler(req, ANT_INT_ANT0); }
static esp_err_t http_external_antenna_handler(httpd_req_t *req) { return http_ant_set_handler(req, ANT_EXT_ANT1); }
static esp_err_t http_auto_antenna_handler(httpd_req_t *req) { return http_ant_set_handler(req, ANT_EXT_ANT1 /*AUTO=>EXT*/); }

static esp_err_t http_ant_status_handler(httpd_req_t *req) {
    ant_mode_t ant;
    ant_status_get(&ant);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ant\":%d}", (int)ant);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t http_ant_clients_handler(httpd_req_t *req) {
    wifi_sta_list_t list = {0};
    esp_err_t err = esp_wifi_ap_get_sta_list(&list);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ap_get_sta_list failed");
        return ESP_OK;
    }
    char json[512];
    size_t off = 0;
    off += snprintf(json + off, sizeof(json) - off, "{\"count\":%d,\"clients\":[", list.num);
    for (int i = 0; i < list.num; ++i) {
        const wifi_sta_info_t *s = &list.sta[i];
        if (i) off += snprintf(json + off, sizeof(json) - off, ",");
        off += snprintf(json + off, sizeof(json) - off, "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d}",
                        s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5], (int)s->rssi);
        if (off >= sizeof(json) - 64) break;
    }
    snprintf(json + off, sizeof(json) - off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 6;   // консервативно
    config.max_uri_handlers = 12;  // регистрируем 9 маршрутов; дефолт (8) не вмещал последний (/ant/clients)

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        server_handle = server;

        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t launch_uri = {
            .uri = "/launch",
            .method = HTTP_GET,
            .handler = launch_handler,
        };
        httpd_register_uri_handler(server, &launch_uri);

        httpd_uri_t cancel_uri = {
            .uri = "/cancel",
            .method = HTTP_GET,
            .handler = cancel_handler,
        };
        httpd_register_uri_handler(server, &cancel_uri);

#ifdef CONFIG_HTTPD_WS_SUPPORT
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &ws_uri);
#endif

        // Антенна API (всегда доступны)
        httpd_uri_t ant0_uri = { .uri = "/ant/ant0", .method = HTTP_GET, .handler = http_internal_antenna_handler };
        httpd_uri_t ant1_uri = { .uri = "/ant/ant1", .method = HTTP_GET, .handler = http_external_antenna_handler };
        httpd_uri_t anta_uri = { .uri = "/ant/auto", .method = HTTP_GET, .handler = http_auto_antenna_handler };
        httpd_uri_t ant_status_uri = { .uri = "/ant/status", .method = HTTP_GET, .handler = http_ant_status_handler };
        httpd_uri_t ant_clients_uri = { .uri = "/ant/clients", .method = HTTP_GET, .handler = http_ant_clients_handler };
        httpd_register_uri_handler(server, &ant0_uri);
        httpd_register_uri_handler(server, &ant1_uri);
        httpd_register_uri_handler(server, &anta_uri);
        httpd_register_uri_handler(server, &ant_status_uri);
        httpd_register_uri_handler(server, &ant_clients_uri);

        ESP_LOGI(TAG, "HTTP сервер запущен на порту 80");
    } else {
        ESP_LOGE(TAG, "Ошибка запуска HTTP сервера");
    }
    return server;
}

static void antenna_init(void) {
    // Ручная схема Seeed для XIAO ESP32-C6: настраиваем управляющие пины
    // RF-переключателя как выходы, подаём питание и выбираем антенну.
    // PHY-API диверсити (esp_phy_set_ant*) для этой платы не подходит — оно
    // кодирует номер антенны в те же GPIO и в итоге снимает питание с
    // переключателя, оставляя встроенную антенну.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ANT_RF_SWITCH_GPIO) | (1ULL << ANT_SELECT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(ANT_RF_SWITCH_GPIO, 0); // запитать RF-переключатель (active LOW)
    vTaskDelay(pdMS_TO_TICKS(100));          // дать переключателю стабилизироваться
    ant_gpio_apply(g_current_ant);           // выбрать антенну (внешняя по умолчанию)

    ESP_LOGI(TAG, "Антенна: RF-переключатель вкл (GPIO%d=0), выбор GPIO%d=%d (%s)",
             ANT_RF_SWITCH_GPIO, ANT_SELECT_GPIO,
             (g_current_ant == ANT_EXT_ANT1) ? 1 : 0,
             (g_current_ant == ANT_EXT_ANT1) ? "внешняя" : "встроенная");
}

void wifi_init_softap() {
    antenna_init(); // выбрать внешнюю антенну до старта радио
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = FIRESTARTER_WIFI_SSID,
            .ssid_len = 0,
            .password = FIRESTARTER_WIFI_PASSWORD,
            .max_connection = 3,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };
    // Пароль задаётся локально (sdkconfig вне git). Если он не задан —
    // поднимаем открытую сеть, иначе WPA2 с пустым паролем не стартует.
    if (strlen(FIRESTARTER_WIFI_PASSWORD) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "Wi-Fi пароль не задан — открытая сеть");
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_country_t country = {
        .cc = "US",
        .schan = 1,        // начальный канал
        .nchan = 11,       // количество каналов (для России 1-13)
        .max_tx_power = 84, // максимальная мощность передачи
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_country(&country));

    int8_t txp_q = 0;
    if (esp_wifi_get_max_tx_power(&txp_q) == ESP_OK) {
        float txp_dbm = ((float)txp_q) * 0.25f;
        ESP_LOGI(TAG, "Wi‑Fi TX power set: %d (%.2f dBm)", (int)txp_q, txp_dbm);
    }
    ESP_LOGI(TAG, "WiFi AP '%s' запущена", FIRESTARTER_WIFI_SSID);
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    gpio_reset_pin(MOSFET_GPIO);
    gpio_set_direction(MOSFET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MOSFET_GPIO, 0);

    ledc_timer_config_t timer = {
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 1000,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .channel = LEDC_CHANNEL_0,
        .duty = 0,
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = LEDC_TIMER_0
    };
    ledc_channel_config(&channel);

    ESP_LOGI(TAG, "Запуск системы...");
    wifi_init_softap();
    start_webserver();

    vTaskDelay(pdMS_TO_TICKS(5000));
    startup_melody();

    ESP_LOGI(TAG, "Система полностью готова");
}
