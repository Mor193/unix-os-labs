# Лабораторная работа №1: Монитор (POSIX Threads)

Реализация потокобезопасного монитора на C++ с использованием библиотеки `pthread` для синхронизации потока-поставщика и потока-потребителя.

## Описание задачи

* Поток-поставщик с интервалом в 1 секунду создаёт условное событие и уведомляет о нём.
* Поток-потребитель ожидает событие, получает его и обрабатывает.
* Сообщения об отправке и получении строго чередуются без нарушения очерёдности.
* Ожидание события реализовано без активного опроса (минимальное потребление CPU).
* Событие представляет собой несериализуемый объект (запрещено копирование, хранит динамически выделенное состояние и монотонную метку времени) и передаётся по указателю.

## Архитектура решения

* **`Event`** — несериализуемый класс события с удалённым конструктором копирования и оператором присваивания; передаётся через монитор по указателю `Event*`.
* **`Monitor`** — инкапсулирует мьютекс (`pthread_mutex_t`), условную переменную (`pthread_cond_t`), предикат готовности `ready`, флаг завершения `stopping` и указатель на данные `data`.
  * `provide()` проверяет отсутствие необработанного события, сохраняет указатель, выставляет `ready = true` и вызывает `pthread_cond_signal()`.
  * `consume()` ожидает событие в цикле `while (!ready && !stopping)` через `pthread_cond_wait()`, что защищает от ложных пробуждений и исключает нагрузку на процессор.
  * `stop()` выставляет `stopping = true` и пробуждает ожидающий поток через `pthread_cond_broadcast()`.
* **`Producer` / `Consumer`** — потоки генерации и обработки событий. Поддерживается корректное завершение по сигналам `SIGINT` (`Ctrl+C`) и `SIGTERM`.

## Сборка

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread main.cpp -o monitor_app
```

## Запуск

Непрерывная работа до прерывания (`Ctrl+C`):

```bash
./monitor_app
```

Запуск на фиксированное количество событий (например, 5) с замером процессорного времени:

```bash
time ./monitor_app 5
```

## Пример вывода

```text
[Producer] Sent event #1 | ptr=0x7ac354000880 | payload=data_1
[Consumer] Received event #1 | ptr=0x7ac354000880 | payload=data_1 | lifetime_us=384
[Producer] Sent event #2 | ptr=0x7ac354000cf0 | payload=data_2
[Consumer] Received event #2 | ptr=0x7ac354000cf0 | payload=data_2 | lifetime_us=231
[Producer] Sent event #3 | ptr=0x7ac354000d50 | payload=data_3
[Consumer] Received event #3 | ptr=0x7ac354000d50 | payload=data_3 | lifetime_us=171
[Producer] Sent event #4 | ptr=0x7ac354000db0 | payload=data_4
[Consumer] Received event #4 | ptr=0x7ac354000db0 | payload=data_4 | lifetime_us=186
[Producer] Sent event #5 | ptr=0x7ac354000e10 | payload=data_5
[Consumer] Received event #5 | ptr=0x7ac354000e10 | payload=data_5 | lifetime_us=222

real    0m5.011s
user    0m0.004s
sys     0m0.004s
```