# Cancellable Launch Countdown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the operator abort a rocket launch from the web UI during the 9-step countdown, so the MOSFET never fires.

**Architecture:** A polled `volatile bool launch_abort` flag is set by a new `GET /cancel` httpd handler. The countdown task (`launch_task`) owns all GPIO/buzzer state and checks the flag itself, sleeping in 50 ms slices so it can bail mid-wait. The cancel window covers the 9 counting steps only; once the "ЗАПУСК!" message is sent, the launch is committed. The UI button toggles between "Запуск" and a red "Отмена".

**Tech Stack:** ESP-IDF (ESP32-C6, RISC-V), FreeRTOS, `esp_http_server` + WebSocket, embedded `index.html` (vanilla JS).

**Important context for the engineer:**
- There is **no automated test framework**. The only automated gate is `idf.py build` succeeding. Behavior is verified on hardware via `idf.py monitor` (log tag `firestarter`) and the served web UI.
- `idf.py` requires the ESP-IDF environment to be exported first (on Windows: the ESP-IDF PowerShell, or run `export.ps1`). If `idf.py` is not on PATH, stop and tell the user — do not claim the build passed without running it.
- All user-facing strings and logs are in **Russian** by project convention. Keep new ones Russian.
- Safety: `MOSFET_GPIO` (GPIO2) drives a real igniter. The whole point of this change is that an aborted countdown must **never** reach the `gpio_set_level(MOSFET_GPIO, 1)` call.
- The HTML is embedded into the binary at build time, so any `index.html` edit requires a rebuild to take effect.

---

## File Structure

- Modify: `main/firestarter.c` — abort flag, cancellable delay helper, `beep()`/`countdown_beeps()` signature changes, `launch_task` abort branch, new `/cancel` handler + registration.
- Modify: `main/index.html` — button mode state, `cancelLaunch()`, `cancelled` message case, red cancel-button styling.

No new files. No structural splits — the project is intentionally single-file firmware.

---

## Task 1: Abort flag + cancellable delay helper + abort-aware `beep()`

**Files:**
- Modify: `main/firestarter.c:30` (state), `main/firestarter.c:117-124` (`beep`)

- [ ] **Step 1: Add the abort flag**

In `main/firestarter.c`, find (line ~30):

```c
static bool launch_in_progress = false;
```

Add immediately after it:

```c
static volatile bool launch_abort = false;
```

- [ ] **Step 2: Add the cancellable delay helper and make `beep()` abort-aware**

Replace the whole current `beep()` function (lines ~117-124):

```c
void beep(const int freq, const int duration_ms) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 128);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
```

with:

```c
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
```

Note: `startup_melody()` calls `beep()` and ignores the return value — that is fine and intended (the C compiler does not warn on a discarded `bool` return). Outside a launch `launch_abort` is always `false`, so the startup melody is unchanged.

- [ ] **Step 3: Verify the firmware builds**

Run: `idf.py build`
Expected: `Project build complete.` and `build/firestarter.bin` produced, with no new warnings/errors. (If `idf.py` is not available, stop and report — do not skip.)

- [ ] **Step 4: Commit**

```bash
git add main/firestarter.c
git commit -m "Add abort flag and cancellable delay/beep primitives"
```

---

## Task 2: Make `countdown_beeps()` and `launch_task()` honor the abort

**Files:**
- Modify: `main/firestarter.c:126-146` (`countdown_beeps`), `main/firestarter.c:148-164` (`launch_task`)

- [ ] **Step 1: Make `countdown_beeps()` return whether it was aborted**

Replace the whole current `countdown_beeps()` function (lines ~126-146):

```c
void countdown_beeps() {
    const int countDown = 9;
    int currentCount = countDown;

    send_ws_message("{\"type\":\"countdown_start\",\"count\":9}");
    vTaskDelay(pdMS_TO_TICKS(1000));

    for (int i = 1; i <= countDown; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"type\":\"countdown\",\"count\":%d}", currentCount);
        send_ws_message(msg);

        beep(300 + i * 100, 600);
        currentCount--;
        ESP_LOGI(TAG, "%d", currentCount);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    send_ws_message("{\"type\":\"launch\",\"message\":\"ЗАПУСК!\"}");
    beep(1500, 2000);
}
```

with:

```c
// Возвращает true, если запуск был отменён во время счёта. Окно отмены —
// только 9 шагов счёта; после сообщения "launch" запуск зафиксирован.
bool countdown_beeps() {
    const int countDown = 9;
    int currentCount = countDown;

    send_ws_message("{\"type\":\"countdown_start\",\"count\":9}");
    if (cancellable_delay_ms(1000)) return true;

    for (int i = 1; i <= countDown; i++) {
        if (launch_abort) return true;

        char msg[64];
        snprintf(msg, sizeof(msg), "{\"type\":\"countdown\",\"count\":%d}", currentCount);
        send_ws_message(msg);

        if (beep(300 + i * 100, 600)) return true;
        currentCount--;
        ESP_LOGI(TAG, "%d", currentCount);
        if (cancellable_delay_ms(1000)) return true;
    }

    // Точка фиксации: дальше отмена не действует.
    send_ws_message("{\"type\":\"launch\",\"message\":\"ЗАПУСК!\"}");
    beep(1500, 2000);
    return false;
}
```

- [ ] **Step 2: Add the abort branch to `launch_task()`**

Replace the whole current `launch_task()` function (lines ~148-164):

```c
void launch_task(void *pvParameters) {
    launch_in_progress = true;
    ESP_LOGI(TAG, "Начинаем обратный отсчет");
    countdown_beeps();

    ESP_LOGI(TAG, "Запуск!");
    gpio_set_level(MOSFET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    gpio_set_level(MOSFET_GPIO, 0);
    ESP_LOGI(TAG, "Пуск выполнен");

    send_ws_message("{\"type\":\"status\",\"message\":\"🚀 Пуск выполнен\"}");

    launch_in_progress = false;
    launch_task_handle = NULL;
    vTaskDelete(NULL);
}
```

with:

```c
void launch_task(void *pvParameters) {
    launch_in_progress = true;
    launch_abort = false;
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

    launch_in_progress = false;
    launch_task_handle = NULL;
    vTaskDelete(NULL);
}
```

- [ ] **Step 3: Verify the firmware builds**

Run: `idf.py build`
Expected: `Project build complete.`, no new warnings/errors.

- [ ] **Step 4: Commit**

```bash
git add main/firestarter.c
git commit -m "Honor abort flag in countdown and launch task"
```

---

## Task 3: `GET /cancel` HTTP handler

**Files:**
- Modify: `main/firestarter.c:184-196` (add handler after `launch_handler`), `main/firestarter.c:283-288` (register route)

- [ ] **Step 1: Add the `/cancel` handler**

In `main/firestarter.c`, immediately after the closing brace of `launch_handler` (line ~196), add:

```c
esp_err_t cancel_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Запрос отмены");
    if (!launch_in_progress) {
        httpd_resp_sendstr(req, "Нет активного запуска");
        return ESP_OK;
    }
    launch_abort = true;
    httpd_resp_sendstr(req, "Отмена запущена");
    return ESP_OK;
}
```

- [ ] **Step 2: Register the `/cancel` route**

Find the `/launch` registration block in `start_webserver()` (lines ~283-288):

```c
        httpd_uri_t launch_uri = {
            .uri = "/launch",
            .method = HTTP_GET,
            .handler = launch_handler,
        };
        httpd_register_uri_handler(server, &launch_uri);
```

Add immediately after it:

```c
        httpd_uri_t cancel_uri = {
            .uri = "/cancel",
            .method = HTTP_GET,
            .handler = cancel_handler,
        };
        httpd_register_uri_handler(server, &cancel_uri);
```

- [ ] **Step 3: Verify the firmware builds**

Run: `idf.py build`
Expected: `Project build complete.`, no new warnings/errors.

- [ ] **Step 4: Commit**

```bash
git add main/firestarter.c
git commit -m "Add GET /cancel endpoint to abort a running launch"
```

---

## Task 4: UI — cancel button mode + `cancelled` handling

**Files:**
- Modify: `main/index.html` — CSS (~line 49), button element (~line 132), JS (`handleMessage` ~168-201, `launchRocket` ~203-206)

- [ ] **Step 1: Add red cancel-button styling**

In `main/index.html`, find the `.launch-btn:disabled` rule (ends ~line 54):

```css
        .launch-btn:disabled {
            background:#666;
            cursor:not-allowed;
            transform:none;
            box-shadow:none;
        }
```

Add immediately after it:

```css
        .launch-btn.cancel-btn {
            background:linear-gradient(45deg, #e53935, #b71c1c);
            box-shadow:0 8px 16px rgba(229,57,53,0.3);
        }
```

- [ ] **Step 2: Replace the button click wiring and add the mode dispatcher**

Find the button element (line ~132):

```html
    <button class='launch-btn' id="launchBtn" onclick='launchRocket()'>Запуск</button>
```

Replace it with:

```html
    <button class='launch-btn' id="launchBtn" onclick='onLaunchBtn()'>Запуск</button>
```

Then find `launchRocket()` (lines ~203-206):

```javascript
        function launchRocket() {
            if (launchBtn.disabled) return;
            fetch('/launch', { method: 'GET', credentials: 'omit' });
        }
```

Replace it with:

```javascript
        // Режим кнопки: 'idle' = Запуск, 'cancel' = Отмена.
        let btnMode = 'idle';

        function setIdle() {
            btnMode = 'idle';
            launchBtn.disabled = false;
            launchBtn.textContent = 'Запуск';
            launchBtn.classList.remove('cancel-btn');
        }

        function setCancel() {
            btnMode = 'cancel';
            launchBtn.disabled = false;
            launchBtn.textContent = 'Отмена';
            launchBtn.classList.add('cancel-btn');
        }

        function onLaunchBtn() {
            if (launchBtn.disabled) return;
            if (btnMode === 'cancel') {
                cancelLaunch();
            } else {
                launchRocket();
            }
        }

        function launchRocket() {
            fetch('/launch', { method: 'GET', credentials: 'omit' });
        }

        function cancelLaunch() {
            fetch('/cancel', { method: 'GET', credentials: 'omit' });
        }
```

- [ ] **Step 3: Update `handleMessage()` for the new states**

Replace the whole current `handleMessage()` function (lines ~168-201):

```javascript
        function handleMessage(data) {
            switch (data.type) {
                case 'countdown_start':
                    launchBtn.disabled = true;
                    countdownEl.classList.add('active');
                    messageEl.classList.remove('visible');
                    break;

                case 'countdown':
                    countdownEl.textContent = data.count;
                    countdownEl.classList.add('active');
                    break;

                case 'launch':
                    countdownEl.textContent = '🚀';
                    messageEl.textContent = data.message;
                    messageEl.classList.add('visible');
                    setTimeout(() => {
                        countdownEl.classList.remove('active');
                        messageEl.classList.remove('visible');
                        launchBtn.disabled = false;
                    }, 3000);
                    break;

                case 'status':
                    messageEl.textContent = data.message;
                    messageEl.classList.add('visible');
                    setTimeout(() => messageEl.classList.remove('visible'), 3000);
                    break;

                default:
                    console.log('Unknown msg', data);
            }
        }
```

with:

```javascript
        function handleMessage(data) {
            switch (data.type) {
                case 'countdown_start':
                    setCancel();
                    countdownEl.classList.add('active');
                    messageEl.classList.remove('visible');
                    break;

                case 'countdown':
                    setCancel();
                    countdownEl.textContent = data.count;
                    countdownEl.classList.add('active');
                    break;

                case 'launch':
                    // Зафиксировано: отмена больше недоступна.
                    btnMode = 'idle';
                    launchBtn.disabled = true;
                    launchBtn.textContent = 'Запуск';
                    launchBtn.classList.remove('cancel-btn');
                    countdownEl.textContent = '🚀';
                    messageEl.textContent = data.message;
                    messageEl.classList.add('visible');
                    setTimeout(() => {
                        countdownEl.classList.remove('active');
                        messageEl.classList.remove('visible');
                        launchBtn.disabled = false;
                    }, 3000);
                    break;

                case 'cancelled':
                    countdownEl.classList.remove('active');
                    countdownEl.textContent = '';
                    messageEl.textContent = data.message;
                    messageEl.classList.add('visible');
                    setTimeout(() => messageEl.classList.remove('visible'), 3000);
                    setIdle();
                    break;

                case 'status':
                    messageEl.textContent = data.message;
                    messageEl.classList.add('visible');
                    setTimeout(() => messageEl.classList.remove('visible'), 3000);
                    break;

                default:
                    console.log('Unknown msg', data);
            }
        }
```

- [ ] **Step 4: Verify the firmware builds (HTML is embedded at build time)**

Run: `idf.py build`
Expected: `Project build complete.`, no new warnings/errors.

- [ ] **Step 5: Commit**

```bash
git add main/index.html
git commit -m "UI: turn launch button into cancel during countdown"
```

---

## Task 5: Hardware verification

There is no automated test for behavior — verify on the device. Flash and watch the serial log + UI.

- [ ] **Step 1: Flash and monitor**

Run: `idf.py -p COM4 flash monitor` (adjust port as needed)
Expected: boot completes, `WiFi AP 'Firestarter' запущена`, startup melody plays. Connect a phone/laptop to the AP and open `http://192.168.4.1/`. The connection chip shows «Подключено».

- [ ] **Step 2: Scenario — cancel mid-countdown (the core fix)**

1. Press «Запуск». The button turns red and reads «Отмена»; the countdown numbers and beeps start.
2. Press «Отмена» partway through.
   Expected: buzzer stops within ~50 ms; UI shows «Запуск отменён»; button returns to orange «Запуск».
   Serial log shows `Запрос отмены` then `Запуск отменён`, and **NOT** `Запуск!` / `Пуск выполнен`. Confirm the igniter/MOSFET did not energize.

- [ ] **Step 3: Scenario — full launch (regression)**

1. Press «Запуск» and let the countdown run to completion without cancelling.
   Expected: at «ЗАПУСК!» the button is briefly disabled (cannot cancel), MOSFET fires for 2 s, serial shows `Запуск!` then `Пуск выполнен`, UI shows «🚀 Пуск выполнен», then button returns to «Запуск».

- [ ] **Step 4: Scenario — `/cancel` with no active launch**

In a browser, hit `http://192.168.4.1/cancel` directly (no launch running).
Expected: response body «Нет активного запуска»; nothing else happens; serial shows `Запрос отмены`.

- [ ] **Step 5: Scenario — cancel after commit (race at "ЗАПУСК!")**

Start a launch and press «Отмена» as late as possible, after the «ЗАПУСК!» message appears.
Expected: the launch still completes (MOSFET fires) — the cancel window is closed once committed, by design.

- [ ] **Step 6: Final commit (if any verification tweaks were needed)**

```bash
git add -A
git commit -m "Verify cancellable countdown on hardware"
```
(Skip if no changes were required.)

---

## Self-Review notes

- **Spec coverage:** abort flag (T1), cancellable delay <100 ms (T1), abort-aware beep (T1), `countdown_beeps` returns bool + commit point (T2), `launch_task` abort branch never touches MOSFET (T2), `/cancel` handler + responses (T3), UI button mode + `cancelled` case + red styling + `cancelLaunch` (T4), all four verification scenarios (T5). All spec sections map to a task.
- **Type consistency:** `beep` returns `bool` everywhere; `countdown_beeps` returns `bool`; `launch_abort` is the single flag name used in firmware; UI uses `btnMode`/`setIdle`/`setCancel`/`onLaunchBtn`/`cancelLaunch` consistently across button wiring and `handleMessage`.
- **No placeholders:** every code step contains the full replacement code.
