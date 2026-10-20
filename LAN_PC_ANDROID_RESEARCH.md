# Borderlands 2: исследование LAN между ПК и Android

**Статус: STOPPED_BY_USER — работа остановлена 19 октября 2026 года по прямому указанию пользователя.** Активная задача вновь относится к переносу DLC. Возобновлять LAN, менять сетевые версии, DLC-маски или схему репликации без нового указания не следует.

Рабочий кооператив PC ↔ Android **не достигнут**. Исправлен и аппаратно проверен поиск LAN v7/v8. Начальное игровое рукопожатие прошло через отдельное диагностическое реле. Затем оригинальные проверки игры последовательно выявили несовпадение версии и набора DLC. Дальнейшее статическое исследование также обнаружило разный порядок сетевых полей объектов. Подмена GUID или простое повышение номера версии не являются готовым исправлением.

## Устройства, входы и сохранения

| Участник | Проверенная конфигурация |
|---|---|
| Смартфон | ADB `abb24fae`, RMX3371, Snapdragon 865 / Adreno 650, Android 15, ARM32 и ARM64, Wi-Fi `192.168.1.102/24` |
| ПК | Физический интерфейс `192.168.1.10/24`; дополнительный VPN-интерфейс не отключался |
| Игра ПК | `C:\Program Files (x86)\Steam\steamapps\common\Borderlands 2\Binaries\Win32\Borderlands2.exe`, Steam build `9218157`, File/Product version `1.0.257.2863302`, нативная игровая ветка **`2.0.0W`** |
| Android | `com.tt2kgames.Borderlands2`, исходная игровая ветка **`1.8.4W`** |
| Сетевые порты | LAN beacon UDP `14001`; ПК gameplay UDP `7777`; Android gameplay UDP `5000` |
| Карта | Базовая `SanctuaryAir_P`, нормальное прохождение |
| Персонаж | Тестовая Maya 80, `Save002E.sav`, с завершённой основной историей; исходные пользовательские слоты сохранены |

Исходная предпосылка об одинаковой последней версии ПК и Android опровергнута локальными бинарными данными и **реальными ответами LAN**: `BORDERLANDS2-2.0.0W` и `BORDERLANDS2-1.8.4W`.

Основные резервные копии находятся в `work/evidence/lan-pc-android-20261019/pc-before/` и в каталоге сохранений предыдущей проверки `work/evidence/save-ai-locations-20261019/savequit-bridge593-b/after-savequit/`. Полный исходный Android INI сохранён как `work/evidence/lan-pc-android-20261019/phone-before-Android-WillowEngine.ini`.

## Последняя проверенная Android-сборка

QA598: `work/build/bl2-arm64-bridge-seed598.apk`, SHA-256:

```text
390A204EED4AB7A5955B2BB50173E98F11039D5CE572E3EDEFF0C4B26C3DFD37
```

Это QA-сборка ARM64-моста с исправлениями сохранений, боеприпасов, оригинальными именами OBB и адаптером поиска LAN. Это **не финальная поставка работающего PC ↔ Android multiplayer**. Финальные экспортированные APK595/594 предшествуют LAN-адаптеру; их нельзя выдавать за содержащие эту новую правку.

Оригинальные имена данных сохранены:

```text
main.31.com.tt2kgames.Borderlands2.obb
patch.31.com.tt2kgames.Borderlands2.obb
russian_voice.31.com.tt2kgames.Borderlands2.obb
```

DLC-прототип при LAN-тестах на телефоне не устанавливался. Содержание основных OBB не менялось.

## Фактическая последовательность аппаратных проверок

Все пути в таблице относятся к `work/evidence/lan-pc-android-20261019/`.

| Проверка | Наблюдение и граница вывода | Доказательство |
|---|---|---|
| Android-формат v8 → ПК | 23-байтный запрос v8 не получил ответа. Позже выяснилось, что ПК требует v7/16 байт; это не доказательство блокировки firewall | `probe-pc14001-host1.json` |
| Правильный v7 → ПК | Реальный SR 213 байт, совпавший nonce, около 11 мс, адрес ПК и `2.0.0W` | `probe-pc14001-host2-v7.json` |
| Оригинальный v8 → Android QA596 | Реальный SR 216 байт, около 115 мс, порт 5000 и `1.8.4W` | `probe-phone14001-host1-v8.json` |
| Прямой Android → ПК | Оригинальный `open 192.168.1.10:7777` завершился возвратом в меню с ConnectionLost | `phone596-direct-pc7777/`, `phone596-direct-pc7777-state/` |
| UDP relay 1, без преобразований | 200 пакетов в обе стороны; мобильный Steamworks-драйвер добавлял 3 байта, ПК отвечал ACK без прогресса | `direct-relay1.json` |
| UDP relay 2, штатный raw driver | Дополнительный заголовок исчез; ПК начал challenge, затем закрыл соединение из-за различного формата HandshakeStart | `direct-relay2-rawdriver.json` |
| UDP relay 3, только нормализация HandshakeStart | Нативные NMT26 → 27 → 28 → 29 прошли; затем реальный Hello8636 получил Upgrade8639 и сообщение об устаревшей версии | `direct-relay3-raw-handshake26.json`, `phone596-raw-normal26/after.png` |
| UDP relay 4, явно экспериментальный Hello8639 | В пересылаемом Hello изменены только два DWORD версии. Следующий реальный отказ — NMT39 / DlcCompatFailure; package GUID и репликация ещё не достигнуты | `direct-relay4-experimental-hello8639.json`, `phone597-experimental-hello8639/after.png` |
| QA597, новый поиск | v7 и контрольный v8 не отвечали из-за неправильной проверки RELRO vtables. Сбой найден, сохранён и исправлен | `QA597_RELRO_GUARD.md` |
| QA598, контрольный v8 | Реальный SR216, совпавший nonce, около 64 мс | `probe-phone598-v8.json` |
| QA598, ПК-совместимый v7 | Реальный SR209, совпавший nonce, около 10 мс; тело мобильной сессии сохранено | `probe-phone598-v7.json` |
| QA598, состояние мира | Реальный Pawn/ViewTarget, видимая SanctuaryAir и отрисованная сцена после загрузки | `phone598-probe-world-state/` |

Полный вход в кооператив, совместная репликация игроков, бой, смена карты и сохранение в кооперативе **не подтверждены**. Настоящий вход ПК как клиента в Android-хост тоже не завершён: начальный URL запуска не дал проверенной сессии, а зелёная консоль WillowConsole превратила команду `open` в локальный `say`. Эти попытки не считаются успешной обратной проверкой.

## Что изменено в исходниках

- `arm64_bridge/src/bl2_lan_discovery_compat.h`: пять строго проверяемых ARM-инструкций меняют только платформенные биты заголовков LAN, а не глобальную `appGetPlatformType`.
- `arm64_bridge/src/bl2_lan_packet_adapter.h`: ограниченное преобразование заголовков v7/v8 с проверками длины, маркера, платформы, game ID и nonce; тело сессии сохраняется побайтно.
- `arm64_bridge/src/bl2_lan_runtime_adapter.h`: два ingress hook, три SendTo hook и cleanup; оригинальный v8-запрос сохраняется, дополнительно посылается v7; на v7-запрос хост отвечает v7.
- Вызовы синхронизированы для ARM64-моста и native ARM32 bootstrap; ordered static patches включают `bridge-static-lan-discovery-compat.patch` и `bridge-static-lan-dual-format.patch`.
- Шесть новых thunk регистрируются после старого ammo hook; прежние SVC ordinal не сдвинуты. Для AOT добавлены пять исходных адресов продолжения. Runtime JIT fallback в static не добавлялся.
- Исправлена ошибочная проверка `IsReadOnlyRange(vtable)`: настоящие vtables находятся в PF_RW/PT_GNU_RELRO. Новая проверка фиксирует точные символы/address points/слоты и отдельно проверяет код методов.

Host positive/negative tests, проверка настоящих SR через production C++ helper, компиляция трёх ABI, ordered static chain и существующий строгий executor для owned veneers прошли. Полная новая AOT-сборка и аппаратный прогон native/AOT после добавления LAN hook **не выполнены**. Независимый review до аппаратного теста не выявил RELRO-ошибку; поэтому его ранний verdict не заменяет последующий аппаратный результат.

Ни нативная версия UE3, ни GGameVersion, ни package GUID, ни DLC compatibility mask в игровых исходниках/бинарниках не подменялись. Per-connection mapper NetFields исследован, но **не реализован**. LAN-изменения остаются в рабочем дереве для возможного возобновления; запрос пользователя относится к staging этого документа, а не к публикации или коммиту всех изменений.

## Состояние после остановки

- Новые LAN-тесты и работа агентов по LAN остановлены. Диагностические relay завершились по собственному лимиту; слушатель 17777 не оставлен.
- Временный Android `NetworkDevice=IpDrv.TcpNetDriver` отменён полным восстановлением сохранённого исходного INI. Он выбирал встроенный raw driver только для диагностики; production-решение выбора драйвера не завершено.
- Диагностические изменения ПК `ConsoleKey`, `ConsoleClassName` и `Suppress=DevNet/DevNetTraffic/DevOnline` отменены восстановлением сохранённых до правки конфигов после завершения процесса игры. Оригинальные Steam-файлы не перемещались и не редактировались.
- QA mailbox очищен. Хорошее завершённое сохранение Android сохранено. Тестовые резервные копии и evidence не удалены.
- Текущая активная работа: перенос DLC Captain Scarlett, особенно корректные mobile MaterialResource/shader maps, затем остальные зависимости, текстуры и звук; данные DLC должны остаться отдельными OBB с именами версии 31.

## Подробные технические приложения

Ниже в этот же файл включены полные текстовые handoff и разборы. Пути внутри включённых разделов сохранены из оригиналов; исходная папка каждого приложения указана перед его текстом. Сырые дампы, JSON-схемы и журналы остаются в проекте по указанным путям. Это архив исследования, не действующее указание продолжать LAN.


---

## Архив: HOST_RESEARCH.md

Исходная папка: `work/evidence/lan-pc-android-20261019`.

# LAN PC ↔ Android: host evidence and narrow discovery fix

This branch initially read local sources, immutable Android inputs, and installed Steam files. Root subsequently authorized bounded host UDP probes, recorded below. It did not start or modify the PC game, access an Android device, change PC configuration, build an APK, or edit STATE.md. Root owns game interaction and final acceptance.

The installed PC executable is `C:\Program Files (x86)\Steam\steamapps\common\Borderlands 2\Binaries\Win32\Borderlands2.exe`: 27,452,248 bytes, FileVersion/ProductVersion `1.0.257.2863302`, Steam build ID `9218157`. Those are locally observed identities, not a claim of compatibility with mobile `1.8.4W`. SHA and selected native strings are in `binary-network-inventory.json`.

## Existing networking and platform replacements

`LocalPlatform.isOnline()` uses the real Android ConnectivityManager. Its license callback and HTTP helpers do not implement remote vendor services. `LocalIdentity` prepares real local save/achievement owners with an empty remote-friends list and no remote token. The native local-session patches call the original `SignInLocally` and avoid treating an already local login as a disconnect. They do not replace LAN searches with fabricated results or disable native Create/Find/Join methods.

ARM64 `thunks_libc.cpp` uses real socket, bind, connect, sendto, recvfrom, select, getsockopt, setsockopt, ioctl, and address-resolution calls. Legacy ARM32 layouts are translated for fd_set, timeval, ifconf/ifreq, hostent, and addrinfo. Native ARM32 skips RegisterLibcThunks and uses its real Bionic socket imports; its import overrides are graphics/VFS/platform mappings. `arm32_network_abi_test.cpp` contains existing layout and UDP tests; this research did not run their network-I/O portion.

The manifest includes INTERNET, ACCESS_NETWORK_STATE, ACCESS_WIFI_STATE and ACCESS_LOCAL_NETWORK. LaunchControls requests ACCESS_LOCAL_NETWORK on API 37+. The current device's actual grant and local network reachability remain runtime checks.

## Discovery configuration and proven native checks

The mobile original and current full cache contain the same network settings. The PC values were checked in DefaultEngine.ini, BaseEngine.ini, and the existing user WillowEngine.ini.

| Setting | Mobile | Installed PC |
|---|---|---|
| NetworkDevice | OnlineSubsystemSteamworks.IpNetDriverSteamworks | same default override |
| LanAnnouncePort | 14001 | 14001 |
| URL Port / PeerPort | 5000 / 5001 | 7777 / 7778 |
| LanGameUniqueId | 0x4DD37BFF | 0x4DD37BFF |
| LanPacketPlatformMask | 0x00000200 | 0x00000041 |
| Subsystem config GameVersion | 1.0.0.0 | 1.0.0.0 |

The mobile IsValidLanQueryPacket at `0x01C84084` and IsValidLanResponsePacket at `0x01C841F0` decode the four-byte big-endian platform field after byte 0 and require a nonzero bitwise intersection with the configured mask. The original Android sender calls `appGetPlatformType` at `0x00FE4258`, which returns `0x200`. The PC validator uses a **different v7 format with a one-byte platform field**, as independently proven later. The original platform selections have no intersection, and the packet formats are also incompatible. Changing only a receiving Android property or platform mask cannot resolve the latter issue.

All three native LAN header writers were traced through GOT slot `0x02F12BAC` to the same platform getter: the base query writer, base response writer, and Steamworks response override. `discovery-original-preimages.json` contains exact original words, target resolution, and immutable ELF SHA `A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814`.

## Implemented mask-only change in QA596

`bl2_lan_discovery_compat.h` changes exactly five ARM instructions, after checking the image size, six symbol locations, seven surrounding words, the resolved original GOT target, and every instruction preimage. Three LAN writers advertise `0x241` (PC `0x41` plus Android `0x200`); two LAN validators accept that mask. The global platform getter, all other platform decisions, LAN format byte 8, game ID, build-ID comparison, UE3 network-version ranges, and payload serialization remain unchanged. No thunk/SVC registration was added.

| RVA | Original | Replacement |
|---|---|---|
| 0x01C8525C | 0xE12FFF33: blx r3 | 0xE3000241: movw r0, #0x241 |
| 0x01C85B78 | 0xE12FFF33: blx r3 | 0xE3000241: movw r0, #0x241 |
| 0x01F90B08 | 0xE12FFF33: blx r3 | 0xE3000241: movw r0, #0x241 |
| 0x01C840EC | 0xE59520E0: ldr r2, [r5, #0xe0] | 0xE3002241: movw r2, #0x241 |
| 0x01C84250 | 0xE59530E0: ldr r3, [r5, #0xe0] | 0xE3003241: movw r3, #0x241 |

The shared installer is called by bridge JNI and native ARM32 bootstrap; the isolated static source has an ordered patch. LLVM assembly roundtrip, exact immutable ELF preimages, rejection before any write for every modified site/context, idempotence, write verification, and all three affected JNI/bootstrap translation units pass. The complete ordered static patch chain also passes. See `discovery-compat-validation.json` and the associated host/ABI logs.

The runtime config property LanPacketPlatformMask still reads `0x200`; the patch changes only the five instructions. Expected outgoing packet bytes 1–4 are `00 00 02 41`.

## Separate checks after platform admission

The LAN validators also compare a build identifier obtained through OnlineSubsystem virtual `+0x148`, not the text GameVersion setting. Protocol research identified UOnlineSubsystem::GetBuildUniqueId and its package-GUID CRC/default or explicit override. Cooked Engine.xxx independently confirms properties `OnlineSubsystem.BuildIdOverride` (IntProperty, export 1726) and `OnlineSubsystem.bUseBuildIdOverride` (BoolProperty, export 1727). Their exact serialized metadata is in `original-network-classes.json`.

Root can inspect the original properties with `getall OnlineSubsystem bUseBuildIdOverride` and `getall OnlineSubsystem BuildIdOverride`. A mobile-format v8 packet can still be rejected by this build-ID gate. The installed PC v7 LAN header has **no build-ID field**, so a build-ID override does not make its 16-byte query compatible with mobile's 23-byte validator. A server appearing in the browser can still fail the later UE3 control handshake. The separate protocol branch proves mobile UE3 range `[8636,8636]` versus installed PC `[8639,8639]`. This mask-only discovery patch does not raise, bypass, or falsify those ranges, and does not claim successful PC co-op.

## Original plain UDP driver and selection

The immutable ELF contains a complete native `UTcpNetDriver`, its native class registration, UClass storage symbol at `0x0301F620`, and vtable at `0x02E223A8`. Relative to the vtable address point, InitConnect at `+0x160` targets `0x01C88498`; InitListen at `+0x164` targets `0x01C885EC`. InitBase is `0x01C880B8` and TickDispatch is `0x01C96460`. The Steamworks driver has its own vtable at `0x02E4A090` and overrides those same slots.

Importantly, Steamworks InitConnect already delegates to UTcpNetDriver::InitConnect when GSteamworksInitialized is false. InitListen delegates to UTcpNetDriver::InitListen when GSteamworksInitialized is false or GWantsNatTraversal is false. Those branch conditions and exact resolved call targets are recorded in `original-udp-backend.json`; the bodies are in `mobile-lan-functions.asm` and `mobile-udp-construction.asm`. Linking RakNet into the mobile library does not establish that every LAN packet goes through RakNet.

UGameEngine::ConstructNetDriver at `0x0146098C` invokes StaticLoadClass with `engine-ini:Engine.Engine.NetworkDevice` (literal `0x02BEB83C`). On failure it tries `engine-ini:Engine.Engine.FallbackNetworkDevice` (literal `0x02BEB864`). It then calls StaticConstructObject. See `driver-selection.json`. PC BaseEngine.ini line 32 itself specifies `NetworkDevice=IpDrv.TcpNetDriver`; its game DefaultEngine.ini overrides that to the Steamworks subclass.

NetworkDevice is therefore an engine-INI lookup in this binary. It was not found as a reflected Engine.xxx property, and no NetDriverDefinitions field was found in the inspected Engine.xxx/config paths. Do not assume that `set Engine NetworkDevice ...` or a UE4-style NetDriverDefinitions setting exists. A future plain-driver experiment should change the actual loaded Engine-INI value and prove that a newly constructed driver is IpDrv.TcpNetDriver; this research did not change any backend setting or validate an Exec syntax for editing the config cache.

## Inputs for root's real test

1. Use the exact QA596 APK identity and the completed base-game character on both sides. Verify LAN mode, actual local IPv4 addresses, and active socket ports; defaults are PC 7777 and mobile 5000, but an occupied port can cause the original driver to select another one.
2. Capture UDP 14001 while each side searches/hosts. Android uses version 8 / 23-byte header, mask `0x241` from QA596, build ID and nonce. This PC uses version 7 / 16-byte header, one-byte platform and nonce, without build ID. Both share game ID `0x4DD37BFF` and SQ/SR tags. Capture a response and its subsequent admission/rejection, not just the presence of a LAN menu.
3. For a direct connection test that bypasses discovery, root can send the original `open PC_LAN_IP:7777` command on the phone once the actual PC port is verified. The reciprocal target uses the observed Android endpoint. Record original failure logs, NMT_Upgrade/ClientOutdated/ServerOutdated, package errors, or a real welcome/travel and visible second player.
4. If normal Steamworks LAN attempts actually choose a remote transport, investigate the existing plain UTcpNetDriver route before implementing a different transport or touching network versions. The PC bifrost DLL and the absence of an obvious PC RakNet magic string do not alone prove raw LAN transport incompatibility.

Current acceptance remains **dual-format host/ABI passed; PC and phone beacon replies independently observed; new APK runtime and cross-device co-op not yet established by this branch**. Root owns gameplay packet capture, all game interaction, runtime acceptance, and any further protocol change. Root authorized the bounded header adapter after the native/captured payload proof. `DUAL_DISCOVERY_SCOPE.md` and `dual-discovery-validation.json` document its final shared/native/static implementation; a subsequent root-owned APK build and runtime QA are still required.

## Authorized bounded host probe

`probe_pc_lan.py --self-check` confirms both exact query formats: mobile v8 `>BII2sIQ` (23 bytes) and PC v7 `>BBI2sQ` (16 bytes). The first real probe sent mobile's v8 format with platform `0x241`, game ID `0x4DD37BFF`, PC-derived build ID `0x8F49B216`, and a fresh nonce to `192.168.1.10:14001`, followed by one broadcast to `192.168.1.255:14001`. A SO_REUSEADDR/SO_BROADCAST listener on UDP 14001 closed after 5.24 seconds. It recorded only its own broadcast SQ and no matching SR response. See `probe-pc14001-host1.json`.

After root confirmed the PC was hosting SanctuaryAir, the correct v7 query `07 01 4D D3 7B FF 53 51` plus a fresh eight-byte nonce received a real **213-byte SR response in 11.3 ms** from `192.168.1.10:14001`. Only one unicast was sent; no broadcast was needed. The response has version 7, platform `0x40`, matching game ID and nonce, and advertises `192.168.1.10:7777` with `BORDERLANDS2-2.0.0W`. See `probe-pc14001-host2-v7.json`. The first probe's lack of SR is now consistent with the independently proven v7 length/version gate, rather than evidence of inactive hosting or a firewall problem.

Root separately obtained a **216-byte v8 SR** from the phone, advertising `192.168.1.102:5000` and `BORDERLANDS2-1.8.4W`; see `probe-phone14001-host1-v8.json`. The two stripped session bodies are 197 and 193 bytes and share the same observed native field layout. The four-byte size difference is entirely the owner-name length. The native source disassembly and a strict offline parser/roundtrip are in `mobile-lan-payload.asm`, `pc-lan-payload.asm`, `lan-payload-comparison.json`, and `verify_lan_payload.py`. This proves these captured discovery bodies can be retained byte-for-byte by a header adapter; it does not prove gameplay replication or all possible settings-data types.

No process, firewall rule, PC configuration, Android device, or persistent socket was changed by this branch. The probe keeps complete received packet hex for independent inspection. Successful beacon replies do not establish a cooperative gameplay result.


---

## Архив: DUAL_DISCOVERY_SCOPE.md

Исходная папка: `work/evidence/lan-pc-android-20261019`.

# Dual v7/v8 LAN discovery: implemented scope and validation

Status: **host and three ABI compile checks pass; runtime QA pending**. After reviewing the captured body proof, root authorized the shared adapter and its native/static callers. `dual-discovery-validation.json` records the final source identities and checks. Root owns APK construction, device installation and the direct gameplay connection test. QA596 contains the earlier five mask instructions only; the new adapter requires a subsequent APK.

QA597 runtime found a regression that the first mock missed: IsReadOnlyRange rejects valid virtual tables in the ELF's PF_RW/RELRO segment. The corrected code uses exact pinned virtual-table address points plus method-slot/code guards; the mock and immutable-segment checks now cover that condition. See `QA597_RELRO_GUARD.md`. The current validation receipt describes this corrected source; QA598 runtime is pending.

## Observed wire contract

| Field | Installed PC v7 | Mobile v8 |
|---|---|---|
| Header bytes | 16 | 23 |
| Version | byte 0 = 7 | byte 0 = 8 |
| Platform | byte 1 | BE32 bytes 1–4 |
| Game ID | BE32 bytes 2–5 | BE32 bytes 5–8 |
| SQ / SR | bytes 6–7 | bytes 9–10 |
| Build unique ID | absent | BE32 bytes 11–14 |
| Nonce | bytes 8–15 | bytes 15–22 |
| Session body | byte 16 onward | byte 23 onward |

The successful host-to-PC probe is `probe-pc14001-host2-v7.json`: one 16-byte unicast, one 213-byte SR in 11.3 ms, matching nonce, platform `0x40`, endpoint `192.168.1.10:7777`. Root's independent phone probe is `probe-phone14001-host1-v8.json`: 216-byte SR, platform `0x241`, endpoint `192.168.1.102:5000`.

## Session payload proof

The immutable mobile functions are base Append/Read at RVAs `0x01C84364` / `0x01C84A38`, and Steamworks Append/Read at `0x01F8DB2C` / `0x01F8E474`. The PC base ProcessLanPacket VA `0x00E50FA0` calls Append at `0x00E46F70` and Read at `0x00E50530`. PC FUniqueNetId serializer at `0x00E41C50` first calls the BE64 writer, then writes exactly 16 raw bytes in a loop (`cmp ebx, 0x10` at `0x00E41CB5`). Mobile writes BE64 from settings+`0x98`, then raw bytes `+0xA0` through `+0xAF`; its reader mirrors this.

The following observed body order is identical on both platforms:

| Order | Wire encoding | Native values / observation |
|---|---|---|
| Host endpoint | IPv4 4 bytes + BE32 port | PC 7777; phone 5000 |
| Connection counts | four BE32 | settings `+0x74`, `+0x78`, `+0x6C`, `+0x70`; both `[3,0,4,0]` |
| Settings flags | nine one-byte booleans | both `[1,1,1,1,1,1,1,0,0]` |
| Game-version string | BE32 byte count + ASCII | 19 bytes on each, `BORDERLANDS2-2.0.0W` versus `BORDERLANDS2-1.8.4W` |
| Owner identity | BE64 + 16 raw platform bytes | identity contents differ; preserve all bytes |
| Owner name | BE32 byte count + ASCII | 16 bytes on PC versus 12 on phone |
| Localized contexts | BE32 count, then BE32 ID/value + advertisement byte | one context, identical values |
| Properties | BE32 count, then BE32 ID + type byte + typed value + advertisement byte | eight properties, all observed type 1 / int32 |

The body lengths 197 versus 193 differ only by the four bytes in the owner name. Property IDs, types and advertisement bytes match. Values for IDs `0x10000010` and `0x10000028` differ; their semantics are not guessed or changed. No body field should be rewritten by discovery adaptation. The game-version strings and actual owner identities remain distinct.

`verify_lan_payload.py` fully consumes both actual captured bodies, verifies header-model roundtrips preserve body bytes and nonce, rejects ten malformed-header cases and every one of 390 truncated bodies. The fixture reader deliberately handles the observed int32 property type only. Other UE3 settings types and native execution of a production adapter remain untested.

## Implemented minimal hook boundaries

The original mobile input SHA-256 is `A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814`. `dual-discovery-preimages.json` records exact words. Two ingress functions and three existing LAN SendTo calls form the complete installed boundary:

| Boundary | RVA | Original word(s) / ABI |
|---|---|---|
| Base ProcessLanPacket entry | `0x01C85AF0` | `E92D47F0 E1A04000`; R0 owner, R1 bytes, R2 signed length |
| Steamworks ProcessLanPacket entry | `0x01F90748` | `E92D4FF0 E1A04000`; same ABI |
| Query SendTo | `0x01C85364` | `E12FFF3C` / `blx ip` |
| Base SR SendTo | `0x01C85D4C` | `E12FFF3C` / `blx ip` |
| Steamworks SR SendTo | `0x01F90D64` | `E12FFF3C` / `blx ip` |

At the three send calls, R0 is FSocket*, R1 bytes, R2 length, R3 bytes-sent output pointer, and the destination FInternetIpAddr* is the fifth argument at caller SP. The original callable is in IP, loaded from socket vtable `+0x28`. A wrapper must preserve the fifth stack argument, callee-saved registers and original return/result semantics.

Both original ProcessLanPacket implementations strip a fixed 23-byte header: base RVAs `0x01C85E08` / `0x01C85E0C`, Steamworks `0x01F9081C` / `0x01F90828`. Altering only the version or validators would therefore feed a wrong body pointer to the native reader. Normalize a validated v7 input into separate bounded storage before entering the original function, allowing these body readers and slices to remain unchanged.

The concrete behavior is:

1. Keep the existing v8 query and its original return path. At the query's LAN-specific SendTo boundary, additionally send one v7 query with platform byte 1, the same game ID and nonce. Keep the same socket and destination. The v7 send must not overwrite the original bytes-sent output or turn a successful v8 search into failure.
2. Incoming v8 packets continue through the original native validator without translation. Incoming v7 packets require exact 16-byte SQ or nonempty SR body, accepted platform intersection, correct game ID/tag and, for SR, the active search's exact nonce. Validate the owner's LAN state (`+0x49`) before adapting.
3. For admitted v7, create a v8-shaped header with its platform expanded to BE32, original game ID/tag/nonce, and the current local BuildUniqueId required by the original v8 parser. This is an explicit v7 admission path: a v7 packet never supplied a remote build ID. Do not describe the inserted local field as verified remote compatibility.
4. Keep a synchronous scoped context for an admitted v7 query while the original ProcessLanPacket builds its SR. At either SR SendTo boundary, translate that response header back to v7, choose platform byte 1, and keep all session body bytes. A v8 query receives the normal v8 response. A bounded thread-local stack and a return-cleanup thunk restore the original LR and free translated storage. Nested v8 processing explicitly shadows an outer v7 context. No global last-peer guess or unrelated UDP transformation is used.
5. Do not mutate the native query/response packet in place. Both original TickLanTasks receive paths pass a maximum `0x200` bytes (`0x01C86050`, `0x01F90EDC`). A translated v7 input therefore needs at most 519 bytes of temporary storage. Reject invalid/oversized packet lengths, pointer arithmetic overflow and unavailable storage before reading or calling native code. Output shrinks by seven bytes.

`arm64_bridge/src/bl2_lan_packet_adapter.h` implements the shared bounded packet helper. `bl2_lan_runtime_adapter.h` contains the guarded installer, frame cleanup and send/receive boundaries. Callers in bridge JNI and native ARM32 append all six new thunks after the pre-existing ammo hook; no earlier registration order is changed. The original ProcessLanPacket executes by continuation instead of a nested whole-function callback. On ARM64, synchronous getter/socket calls use the existing `CallGuest` API and reject an unsupported callback depth rather than silently defer a required result; native ARM32 uses its real native callback ABI.

The first two instructions of both ingress functions are position independent; a four-word owned veneer executes them then resumes the original at +8, keeping all later PC-relative instructions at their original addresses. Send handlers preserve the original fifth argument and reproduce their one displaced CMP/load before resuming at +8. The installer checks all five two-word preimages and symbols before any registration/write, verifies destination code/vtable and LAN socket/owner relationships, verifies writes and accepts a fully installed idempotent recheck.

The isolated source uses the ordered `bridge-static-lan-dual-format.patch`. Five source-proven original continuation descriptors are appended to `static_recompiler/profiles/bl2-confirmed-seeds.txt`: `41C85AF8`, `41F90750`, `41C8536C`, `41C85D54`, `41F90D6C` (guest base included). Root's normal static builder must regenerate the offline supplement from these seeds. No JIT fallback was added.

## Required checks before any release claim

Completed host checks cover production C++ v8 byte identity, one extra SQ, v7 SR body preservation, nonce and malformed-length rejection, allocation failure, nested owner-context shadow/cleanup, exact fifth argument forwarding, six appended mock thunk identities, preimage/symbol/image/range guard rejection and idempotence. Both actual PC/phone SR captures also pass through the production C++ helper while preserving every body and nonce byte. The strict static owned-veneer executor executes the real ingress prologue/literal-jump shape successfully. Bridge ARM64, native ARM32 and isolated static ARM64 callers compile; `prepare.py --check` validates the entire ordered patch chain. See `dual-host-check.log`, `dual-captured-fixtures.log`, `dual-static-glue-check.log`, `dual-abi-compile.log` and `dual-static-patch-check.log`.

Still required: regenerate/build the static supplement; execute the new APK on the phone; prove the original native game readers admit translated captures and populate the real browser; test PC-host/phone-search and phone-host/PC-search. Gameplay connection/version/package acceptance is a separate root-owned test.

This adapter does not modify global appGetPlatformType, PC files/configuration, package GUIDs, game-version strings, BuildIdOverride properties or UE3 network-version ranges. The original ranges remain mobile `[8636,8636]` versus installed PC `[8639,8639]`; parent is investigating the actual gameplay packet framing independently. An admitted advertisement alone cannot establish cooperative gameplay.


---

## Архив: QA597_RELRO_GUARD.md

Исходная папка: `work/evidence/lan-pc-android-20261019`.

# QA597 LAN regression: concrete RELRO guard correction

Root observed no v7 reply and no control v8 reply from the phone while QA597 was in SanctuaryAir. Captures are `probe-phone597-v7.json` and `probe-phone597-v8-control.json`. This branch subsequently identified an unconditional mismatch between its new guard and the real loader metadata; it did not access the device.

`ElfLoader::IsReadOnlyRange` accepts only readable PT_LOAD ranges without PF_W. The immutable ELF has code in RX PT_LOAD `0x00000000..0x02D1F9B8`, and data in RW PT_LOAD starting `0x02D209D0`. The relevant virtual tables are inside PT_GNU_RELRO `0x02D209D0..0x02F3A000`, but the loader predicate still classifies them by the RW load segment. Thus the original adapter's read-only-vtable check always rejected the valid subsystem and socket tables. The socket rejection affected the original v8 response path too.

| Object table | Exact address point RVA | Virtual slot | Original target RVA |
|---|---:|---:|---:|
| UOnlineSubsystem | 0x02DEF5A0 | +0x148 | 0x01A7C468 |
| UOnlineSubsystemCommonImpl | 0x02E24658 | +0x148 | 0x01A7C468 |
| UOnlineSubsystemSteamworks | 0x02E4A5B0 | +0x148 | 0x01A7C468 |
| FSocketBSD | 0x02E46F50 | +0x28 | 0x01E0F9EC |
| FSocketSteamworksClient | 0x02E494C0 | +0x28 | 0x01F4B388 |
| FSocketSteamworks | 0x02E4A940 | +0x28 | 0x01F4B388 |

The getter assumption itself was correct: Steamworks slot RVA `0x02E4A6F8` has original word `0x01A7C468` with an R_ARM_RELATIVE relocation. Host state values are also correct: StartLanBeacon writes state 1 at `+0x49` after successful initialization (`0x01CACD80` / `0x01CACD8C`), while FindLanGames writes state 2 at `0x01C853A4`.

The correction is local to `bl2_lan_runtime_adapter.h`. It checks those exact immutable-symbol identities/address points and the expected relocated getter/socket method slot. Method destinations still require the original code read-only check. The global ELF loader classification, gameplay protocol, network-version values, game-version strings and BuildIdOverride properties are unchanged. No thunk registration or continuation root was added beyond QA597.

The initial host mock incorrectly treated the complete ELF image, including data virtual tables, as read-only. It now models the actual RX boundary and RW virtual-table addresses. The regression test explicitly proves that the old predicate rejects both real table locations while the new exact-table guard accepts them. `check_dual_discovery.py` independently reads all six table symbols, slots and PT_LOAD/PT_GNU_RELRO membership from the hash-pinned immutable ELF.

The corrected shared/native/static sources again pass host runtime and negative tests, production C++ roundtrips of both captured SR packets, the strict static owned-veneer executor, complete ordered static-patch validation and compilation of the three callers. Final identities are in `dual-discovery-validation.json`. The old QA597 host-only receipt is retained as `dual-discovery-validation-qa597-host-only.json`; it is historical evidence, not a runtime acceptance result.

Root owns QA598 build/install and must repeat both v8 control and v7 probe. Runtime recovery and cooperative gameplay remain unconfirmed by this branch until those tests complete.


---

## Архив: protocol-compat/PROTOCOL.md

Исходная папка: `work/evidence/lan-pc-android-20261019/protocol-compat`.

# PC / Android LAN protocol compatibility: binary evidence

**Research stopped by explicit user steering.** Latest complete runtime/NetFields handoff: `LAN_HANDOFF_STOPPED.md`. Later relay3/relay4 evidence supersedes the earlier runtime boundary below: the native outdated-client gate and the subsequent NMT39 DLC bitset rejection were both actually observed. No production mapper or package alias was implemented.

Status: **three independent compatibility boundaries are proven: discovery v7/v8 layouts, a Steamworks connection's three-byte gameplay header, and non-overlapping native UE3 version ranges**. The root's real relay capture reaches bidirectional UDP but does not establish a control-message version rejection or cooperative gameplay. This analysis branch performs offline analysis only; the root and network agent own all network/device/process actions.

## Exact build and network identifiers

| Field | Original Android | Installed PC |
|---|---|---|
| Game-branch string used by native code | `1.8.4W` | `2.0.0W` |
| UE3 current network version | `8636`, ELF RVA `0x02F3C15C` | `8639`, PE VA `0x019B987C` |
| UE3 minimum network version | `8636`, ELF RVA `0x02F3C154` | `8639`, PE VA `0x019B9884` |
| Supported version interval | `[8636, 8636]` | `[8639, 8639]` |
| Built-from changelist | `1134170` | `2863302` |
| PC file/product version | — | `1.0.257.2863302` |

The PC file version and game-branch string are different identifiers. The PC branch string is established by its FString constructor at VA `0x015AB8F0`, using ASCII literal `2.0.0W` at VA `0x01656398`, into global FString `0x019F2A94`. It is not guessed from a historical patch number.

Immutable Android ELF: 49,885,152 bytes, SHA-256 `7857316c39c0421ae421553bf34c387e53ff671da759bb83c42fe79e1e17264d`. Installed PC EXE: 27,452,248 bytes, SHA-256 `1231b384aea791bc51286664627d03e27d6a77ffd4ec3bb5001186c136504c17`.

## What the version gate actually checks

The native host accepts a remote Hello only when:

```text
remote_current >= local_minimum
AND
remote_minimum <= local_current
```

On failure it sends control message **2, NMT_Upgrade**, containing the local minimum/current values, then closes the connection. The negotiated version on success is the smaller current version.

Android host: `UWorld::NotifyControlMessage` RVA `0x017CBA54`, Hello case `0x017CCED0`, comparisons at `0x017CCF18` and `0x017CCF30`, upgrade call at `0x017CD2D4` through `FNetControlMessage<2>::Send` RVA `0x017DC1C0`.

PC host: native handler VA `0x00AD49D0`, Hello receive VA `0x00AD54D2` through `0x00ABAA60`; comparisons/branches at `0x00AD5510` / `0x00AD5522`; upgrade send through VA `0x00AC9150` at `0x00AD5657`. Both PC globals equal 8639. See `pc/exe-protocol-proof.json` and `pc/server-hello-version-gate.asm` for byte assertions and VA/RVA distinctions.

Consequently, if control-message exchange is reached with the original ranges:

- **Android client → PC host:** Android receives the `ClientOutdated` rejection, because its current version 8636 is below the host minimum 8639.
- **PC client → Android host:** PC receives the `ServerOutdated` rejection, because its required minimum 8639 exceeds the host current 8636.

Android client rejection handler is `UNetPendingLevel::NotifyControlMessage`, RVA `0x01B074AC`. NMT_Upgrade case reads its two 32-bit values at `0x01B078D4` / `0x01B078EC`; local-current comparison is `0x01B07908`; strings are `ClientOutdated` RVA `0x02BFDC44` and `ServerOutdated` RVA `0x02BFDC54`. This predicts the result after reaching this gate; it is not a claim that discovery or an actual test has reached it yet.

The Android Hello helper's real signature includes two int32 versions, one additional uint32 field, and two **16-byte `FDlcCompatibilityData` structures**. It serializes 44 field bytes before the control-channel/packet framing. These last fields should not be mislabeled as arbitrary GUIDs simply because their comparison is four DWORD XORs. The third uint32 field's semantics have not been assigned by this branch.

## Discovery has different wire layouts

The installed PC uses a **16-byte v7** query; Android uses a **23-byte v8** query. Their shared Game ID is `0x4DD37BFF`, but the platform field widths and the Build ID field differ:

| Offset / field | PC v7 | Android v8 |
|---|---|---|
| Version | byte 0 = 7 | byte 0 = 8 |
| Platform | byte 1 | bytes 1..4, BE32 |
| Game ID | bytes 2..5, BE32 | bytes 5..8, BE32 |
| Query / response marker | bytes 6..7, SQ / SR | bytes 9..10, SQ / SR |
| Build ID | absent | bytes 11..14, BE32 |
| Nonce | bytes 8..15 | bytes 15..22 |
| Response settings payload | starts at 16 | starts at 23 |

PC query validator VA `0x00E40840` requires exactly 16 bytes and version 7; response validator VA `0x00E40930` requires more than 16 bytes. Android validators RVA `0x01C84084` / `0x01C841F0` require the v8 layout. Changing only the platform mask or Build ID cannot translate these formats. A PC query with nonce `0102030405060708` is exactly `07014dd37bff53510102030405060708`.

The network agent's root-authorized single-unicast probe using that native v7 format succeeded: a 213-byte SR arrived from `192.168.1.10:14001` after 11.3 ms, with matching nonce, version 7, platform `0x40`, the common Game ID, and session payload advertising game port 7777 and `BORDERLANDS2-2.0.0W`. Evidence: `../probe-pc14001-host2-v7.json`, `pc/lan-discovery-proof.json`. The earlier v8 probe's silence was explained by the wrong layout, rather than proving a firewall failure.

The session payload was independently decoded by the network agent from the actual PC and phone SR captures: the same field schema consumes both bodies fully. See `../lan-payload-comparison.json`. This is useful for a bounded v7/v8 beacon adapter; it does not prove gameplay replication compatibility.

## Android v8 Build ID derivation

The CPU/network agent owns LAN beacon platform masks, packet identifiers, sockets and their actual test. This branch independently resolved the discovery version getter: Steamworks vtable definition RVA `0x02E4A5A8`, address point `+8`, virtual offset `0x148`, slot `0x02E4A6F8` points to **`UOnlineSubsystem::GetBuildUniqueId` RVA `0x01A7C468`**.

It returns `BuildIdOverride` (`this+0xC8`) when `bUseBuildIdOverride` (`this+0xC4`, bit 0) is true. Otherwise it gets the outer package of `UEngine::PrivateStaticClass` and computes **`appMemCrc(package Guid, 16, 0)`**. Exact cooked property names are confirmed in the coordinator's `../original-network-classes.json`. Thus this discovery field is **not** EngineVersion 8636/8639, nor the shared `LanGameUniqueId`.

On the original Android input, and conditional on no override and the loaded Engine GUID matching the source summary, the default is **`0x5113BC36` / `1360247862`**. The native ARM CRC processes each aligned DWORD from its high byte to low byte, using polynomial `0x04C11DB7`; this is not `zlib.crc32` of the raw 16 memory bytes. `mobile-build-id-proof.json`, `mobile-build-unique-id.asm`, `mobile-memcrc.asm` and `mobile-crc-table-init.asm` preserve that derivation.

The PC implementation was then verified independently: `GetBuildUniqueId` VA `0x00CC4E00` has the same override offsets and calls `appMemCrc` VA `0x004779C0` on the Engine package GUID. Its initializer at VA `0x00480B40` establishes the same polynomial, but its DWORD loop processes **low byte to high byte**. The actual calculated PC default is therefore **`0x8F49B216` / uint32 `2403971606` / int32 `-1890995690`**, under the same no-override/source-GUID conditions. The earlier ARM-algorithm-on-PC-GUID value in the mobile derivation is labeled hypothetical and is not this PC result. Evidence: `pc/build-id-proof.json` and its source disassembly.

These values describe each engine's getter. **PC v7 discovery does not carry or compare this Build ID**, so a phone `BuildIdOverride` alone is not a solution for this PC host. Android v8 peers still have the additional Build ID filter.

## Actual gameplay capture: the extra three-byte Steamworks header

The root's bounded relay capture `../direct-relay1.json` contains 200 bidirectional UDP datagrams. The first phone datagram is `0000000080052080a0000d00010080`; a later short one is `0000000140`. PC replies such as `0040002000100008` are valid native UE3 ACK packets. The child's offline decoder parses all 66 PC packets completely and round-trips every packet byte exactly; all ACK IDs are zero. Evidence: `pc/pc-wire-capture-analysis.json`.

The phone prefix is **not a BE32 packet ID**. Removing exactly the first three bytes makes all 134 phone datagrams valid under the PC packet framing, with increasing packet IDs 0 through 133. All 134 then round-trip byte-for-byte: 119 empty native UE3 packets and 15 complete reliable/open control-channel-0 bunch packets with channel sequence 1, 40 data bits and payload `1a00020000`. No other control payload or Hello occurs in this capture. Removing four bytes or reversing bit order does not explain it. Evidence: `pc/phone-strip3-capture-analysis.json`, `pc/phone-candidate-transform-probe.json`.

The original Android source bytes explain those three bytes precisely:

- `UNetConnection::PreSend` RVA `0x01AFE768` checks `this+0x502C`; when nonzero, it serializes three raw bytes from `this+0x5030` before writing the normal packet ID.
- The actual packet-ID helper is `FBitWriter::WriteIntWrapped` RVA `0x00F5AC8C`, reached through GOT slot `0x02F14EB0`. It writes 14 LSB-first bits for maximum 16384. Its `GShift` table at `0x02CB6800` is `01 02 04 08 10 20 40 80`, agreeing with PC. Core bit order and packet-ID width are not the source of this mismatch.
- `UIpNetConnectionSteamworks` constructor RVA `0x01F5A9A8` sets `this+0x502C=1` at `0x01F5AA1C`; its `InternalConstructor` RVA `0x01F732B8` duplicates that initialization at `0x01F73328`. `ReceivedPacket` also expects the three-byte header when that flag is true.
- The raw `UTcpipConnection::InternalConstructor` RVA `0x01CA6B34` does not enable the flag. Steamworks inherits `UTcpipConnection::LowLevelSend` RVA `0x01C71940`; the raw `FSocketBSD::SendTo` RVA `0x01E0F9EC` forwards the buffer directly to `sendto`.

The initial phone control bunch, after header removal, has a five-byte payload `1a00020000` under the 13-bit bunch-size bound (native default maximum packet size 768 bytes). Type 26 is the mobile native `HandshakeStart`, established by `FNetControlMessage<26>::Initialize` RVA `0x01AF9A5C` and its name literal `0x02BFD880`. Its DWORD is **platform type 512 / `0x200`**, not the packet size: the pending-level constructor calls `appGetPlatformType` RVA `0x00FE4258` at `0x01B072B4`, stores its returned DWORD at `0x01B072C4`, and passes it to Send26 at `0x01B072D0`. It is not an observed 44-byte `NMT_Hello`.

This initial control message has a second concrete schema difference: Android `FNetControlMessage<26>::Send` RVA `0x010C41A0` serializes a **uint32 platform field**; PC Send26 VA `0x005612F0` serializes a **single platform byte**. PC's callers use `appGetPlatformType` VA `0x004809E0` and take its low byte; the actual installed host reports platform `0x40`. PC's default-discard handler for type 26 at VA `0x00D1F54D` consumes one byte after its challenge-generation branch. Thus removing the three-byte packet header is necessary but does not establish compatible control-message parsing: the Android DWORD leaves extra bytes under the PC schema. A globally changed platform getter would affect unrelated systems and is not justified; any further diagnostic normalization must be restricted to this control-message path and keep versions/package checks intact.

The smallest next runtime diagnostic uses the original raw backend (`Engine.Engine.NetworkDevice=IpDrv.TcpNetDriver`, whose original `NetConnectionClassName=IpDrv.TcpipConnection`) with original versions retained. The root owns the temporary config change, backup, cold launch and capture. Success must be measured by the actual selected connection class and progression of packet/control traffic; no version or package GUID bypass is justified by this capture.

## Package/content checks remain after the first gate

`compare_script_guids.py` read the original cooked inputs without rewriting any package. The GUIDs of `Core`, `Engine`, `GameFramework` and `WillowGame` differ between the two supplied builds. `Sanctuary_P` has an identical GUID, so it would also be incorrect to declare all package GUIDs different merely because platforms differ. Exact GUID words, package identities, versions and generations are in `script-package-identities.json`.

Android `UWorld::VerifyPackageInfo` RVA `0x017C8C28` has a genuine 16-byte loaded-package GUID comparison at `0x017C8CC8` onward; the initial `GUseSeekFreeLoading` value is 1. The mismatch branch resolves the localized `Core.PackageVersion` key, and the control-message route also contains `Package '%s' version mismatch`. This is a concrete later policy boundary. A static source-GUID difference alone does not prove that a particular runtime package message has already failed.

The Hello also contains DLC compatibility data, and PC has distinct post-version rejection routes. Changing only a version number cannot establish that package maps, native/script replicated fields, DLC flags and actual gameplay replication agree.

## RakNet versus PC transport: keep the layers separate

Android contains a real **RakNet outer protocol byte 6**: `RakNet::ProcessOfflineNetworkPacket` RVA `0x01EB3248` reads request datagram byte 17 at `0x01EB5484`, compares against 6 at `0x01EB5488`, and branches on mismatch to `0x01EB6118`. It constructs a response with ID **25**, expected byte **6**, the 16-byte offline magic `00ffff00fefefefefdfdfdfd12345678`, and the 8-byte server GUID. Relevant opcodes and magic bytes are verified by `verify_mobile_handshake.py`.

The 16-byte BSS symbol `GRaknetClientGameVersion` is a separate structure at RVA `0x03065944`. `AndroidSteam_SetGameVersion` at RVA `0x01F5291C` accepts four uint32 values and stores them there. A BSS symbol and setter existence do **not** establish the active values or prove that a particular LAN path uses this outer transport.

The PC EXE imports Bifrost socket/P2P APIs and also has ordinary WSOCK32 UDP wrappers. It does not contain the standard RakNet offline magic or RakNet strings. **PC outer RakNet protocol compatibility is not established** by the common UE3 code or the Android byte value. The installed Bifrost dependency is being analyzed separately under `pc/`; its raw LAN selection/framing must be distinguished from P2P transport. No protocol number is invented when the corresponding PC gate has not been found.

## Reproducible checks

Run from the project root, selecting new output paths because reports refuse overwriting:

```powershell
& 'C:\Program Files\Python313\python.exe' -X utf8 -B .\work\evidence\lan-pc-android-20261019\protocol-compat\verify_mobile_handshake.py --output .\work\evidence\lan-pc-android-20261019\protocol-compat\mobile-handshake-recheck.json
& 'C:\Program Files\Python313\python.exe' -X utf8 -B .\work\evidence\lan-pc-android-20261019\protocol-compat\compare_script_guids.py --output .\work\evidence\lan-pc-android-20261019\protocol-compat\package-identities-recheck.json
& 'C:\Program Files\Python313\python.exe' -X utf8 -B .\work\evidence\lan-pc-android-20261019\protocol-compat\calc_mobile_build_id.py --output .\work\evidence\lan-pc-android-20261019\protocol-compat\build-id-recheck.json
& 'C:\Program Files\Python313\python.exe' -X utf8 -B .\work\evidence\lan-pc-android-20261019\protocol-compat\verify_mobile_packet_header.py --output .\work\evidence\lan-pc-android-20261019\protocol-compat\packet-header-recheck.json
```

Established runtime milestones: native v7 PC discovery response, native v8 phone discovery response, and bidirectional direct-IP UDP with a precisely diagnosed extra three-byte Steamworks header. Not established: successful cross-version in-game discovery, a control-message version rejection, package-map acceptance or working cooperative gameplay. Those remain runtime milestones owned by the coordinator.


---

## Архив: protocol-compat/LAN_HANDOFF_STOPPED.md

Исходная папка: `work/evidence/lan-pc-android-20261019/protocol-compat`.

# LAN research handoff — stopped by explicit user steering

Status: **STOPPED_BY_USER**. No LAN device/process/network action was performed by this agent. The root owns all runtime actions and the overall stage file. No production gameplay mapper, package alias, DLC compatibility spoof or source version override was implemented by this branch. The isolated relay is not running from this agent.

## Actual runtime milestones

1. `../probe-pc14001-host2-v7.json`: native PC v7 discovery response, 213 bytes, matching nonce, platform 0x40, game ID 0x4DD37BFF, port7777, game branch2.0.0W. Android discovery uses v8/23-byte headers; PC uses v7/16-byte headers. PC v7 contains no BuildUniqueId.
2. `../direct-relay1.json`: 200 bidirectional UDP datagrams. Mobile Steamworks connection adds three header bytes; all134 phone packets become exact native UE3 packets after removing precisely three bytes. This was not a 32-bit packet-ID or bit-order difference. Both native bit writers use14 LSB-first bits.
3. `../direct-relay2-rawdriver.json`: root cold-selected original `IpDrv.TcpNetDriver`, and runtime readback confirmed `IpDrv.TcpipConnection`. Header issue disappeared. PC sent native HandshakeChallenge27 then closed42–70 microseconds later, before the phone response. Mobile HandshakeStart26 serialized a DWORD platform0x200, while PC expects one byte. Challenge-response CRC already matches exactly across the two builds; no CRC patch is needed.
4. `../direct-relay3-raw-handshake26.json`: root ran the bounded exact-schema relay. Only HandshakeStart26 DWORD0x200→BYTE0x40 was normalized. Native26→27→28→29 completed. Actual phone Hello45B had min/current8636, additional DWORD0, two zero16B FDlcCompatibilityData structures. PC replied with native Upgrade2 min/current8639; UI showed the corresponding outdated-client error. All110 datagrams round-trip through the offline decoder.
5. `../direct-relay4-experimental-hello8639.json`: explicitly authorized diagnostic flag additionally changed only the exact observed zero-DLC Hello's two version DWORDs in forwarded traffic to8639. Original real identity8636 remains in the receipt; source binaries were unchanged. Next actual rejection is **NMT39 / DlcCompatFailure**, with PC FDlcCompatibilityData bytes `0000000000000000000000007fabbf00`, followed by close. All92 datagrams round-trip. This is before package GUID checks and cooperative gameplay.

The root's latest stable PC process and device/build state must be read from its stage evidence; this agent does not own them. A phone pre-experiment config backup exists at `../phone-before-Android-WillowEngine.ini`; the root owns restoration decisions.

## Exact binary conclusions

- Original mobile ELF SHA256 `7857316c39c0421ae421553bf34c387e53ff671da759bb83c42fe79e1e17264d`; PC EXE SHA256 `1231b384aea791bc51286664627d03e27d6a77ffd4ec3bb5001186c136504c17`.
- Mobile native version interval8636..8636; PC8639..8639. Exact native comparisons are recorded in `mobile-handshake-summary.json` and `pc/exe-protocol-proof.json`.
- Mobile Steamworks constructors enable connection+0x502C; PreSend serializes three bytes from+0x5030. Original raw TcpipConnection does not enable it. `mobile-packet-header-and-start-proof.json` is the pinned opcode/GOT/GShift verifier result.
- PC Hello and mobile Hello both have44 field bytes plus control ID, with two16B FDlcCompatibilityData structures. A bounded PC code immediate search found no genuine8637/8638/8639 threshold, which does not establish serializer compatibility.
- DLC data is a package bitset, not an arbitrary GUID. Mobile native IsPackageFlagSet0200AAA4 uses PackageId-1, reversed DWORD groups, with IDs1..32 in data+12. PC mask0x00BFAB7F activates IDs1,2,3,4,5,6,7,9,10,12,14,16,17,18,19,20,21,22,24. Orchid's installed Compat INI identifies PackageId1. No stock PC DLC-disable option was proven before the stop instruction. No Steam content was moved or edited.

## Script schemas and mapping research

Authoritative normalized source evidence is in `mobile-schemas-v3/schema-mobile-*.json` and `pc/schema-pc-*-v3.json`, covering Core, Engine, GameFramework, GearboxFramework and WillowGame. All standard and attribute property tails and all function bodies parse without trailing bytes/errors. PC editor-only category/array-enum/operator/friendly-name data was normalized away using native layout evidence, rather than treating raw-body hashes as semantic mismatches.

- `pc/network-schema-comparison-five-packages.json` reports112 classes with own NetFields and14 own-order differences. Player inheritance resolves only after including GearboxFramework.
- `Engine.Actor` has the same24 own names but a different wire order: first divergence PC bBlockActors / mobile RelativeRotation. WillowPlayerController has303/298 own fields, WillowPlayerPawn27/19. Exact missing-field/RPC/type details are in the child's comparison artifacts; counts alone do not identify initial-join traffic.
- Native `UClass::Link`00F682F0 collects net properties/root net functions and sorts by **serialized UObject NetIndex** through Sort00F80714. This is not alphabetical sorting. `SerializeNetIndex`00FFD5E0 preserves serialized indices when package flag8 is set. All five original PC and mobile cooked packages have flag8. This makes the order divergence relevant to runtime, rather than an unverified source-table observation.
- A per-connection shadow FClassNetCache was researched only. GetClassNetCache00F6E6A0 receives PackageMap/Class; SeekFree PackageMap+C0 is its connection, connection+60 driver, connection+AC backreference. GetFromIndex01B123B4 and GetFromField00F7E98C are candidate mapping points. Cache size0x64, fields-array+1C/count+20/base+0C/super+10, entries12B. The separate RepProperties pointer list and UField→cache map must also point at mapped entries. Inline MaxIndex uses base+count, so mapping returned indices alone is insufficient.
- GetFromIndex's first8 bytes include a PC-relative conditional branch: blindly copying them into a trampoline is unsafe. Its common path01B123CC starts with LDR/CMP and is a possible safer guarded site. Nothing was patched.
- SeekFree ObjectToIndex01B10058 and IndexToObject01B100D0 are fatal unsupported stubs. Actual SeekFree SerializeObject01B0CC30 uses dynamic actor-channel IDs or -1 plus full object path for static references. A blind base ObjectToIndex hook would target the wrong path.
- No package GUID alias was approved or implemented. Core/Engine/GameFramework/WillowGame source GUIDs differ; Sanctuary_P matches. Full co-op requires typed shared-field mapping, inherited bounds, explicit rejection of unsupported fields, actual object/content acceptance and runtime replication validation.

## Isolated experimental tools

`handshake26_normalizer.py` and `udp_relay_handshake26.py` are diagnostic tools under this evidence directory. Default mode changes only the exact observed HandshakeStart. The Hello negotiation mode requires explicit `--experimental-pc-hello8639`, rejects different control IDs, third fields, nonzero DLC data, channels, sequence or reliability, and preserves all ACK/packet/bunch metadata. Offline positive/negative checks passed; receipts distinguish real source identity from forwarded diagnostic values. Neither tool was executed on the network by this agent, and neither is a production compatibility implementation.

Next active work has returned to the original DLC material/shader port under `dlc_port/`; LAN research is stopped.


---

## Архив: protocol-compat/pc/RESULT.md

Исходная папка: `work/evidence/lan-pc-android-20261019/protocol-compat/pc`.

# PC protocol evidence, 2026-10-19

**Static analysis complete; no PC process, socket, Android device or network was accessed by this agent.** All installed inputs remained read-only. Addresses below are preferred PE VAs; subtract the image base before relocating to an ASLR runtime address.

The decisive discovery difference is **PC LAN header version 7, query length 16 bytes**. This PC does not put `BuildUniqueId` into its LAN beacon header. A 23-byte version-8 mobile query is rejected by PC before any UE3 handshake or build-CRC comparison.

## Inputs

| Input | Bytes | SHA256 |
|---|---:|---|
| Installed `Binaries/Win32/Borderlands2.exe` | 27,452,248 | `1231b384aea791bc51286664627d03e27d6a77ffd4ec3bb5001186c136504c17` |
| Installed `Binaries/Win32/bifrost.dll` | 8,488,448 | `83be32e90b54b8e49adcac04f9a8cdcae3653b6ef445cb47a8ebf6906d0dff85` |
| Installed `Binaries/Win32/steam_api.dll` | 105,808 | `b55054a9d9287c704b8e0ad3acfef1ea5c3fa6982b20b7e36466dc76a6ad8925` |
| Installed `WillowGame/CookedPCConsole/Engine.upk` | full LZO wrapper | `d3debcd4236dfe4084fd3d1950651f32aed4aeebf486f38f7434af3fdecb1253` |

PC EXE image base is `0x00400000`; Bifrost DLL image base is `0x10000000`.

## LAN discovery: exact PC format

`IsValidLanQueryPacket` at `0x00E40840` checks length **exactly 16**, version **7**, a **one-byte** platform mask, game ID, and `SQ`. It extracts the following nonce. `IsValidLanResponsePacket` at `0x00E40930` requires length **greater than 16**, the corresponding `SR` marker, and nonce equality with the local outstanding query.

| Offset | Bytes | Field | PC check |
|---:|---:|---|---|
| 0 | 1 | Version | `7` |
| 1 | 1 | Platform | `(packet[1] & BYTE(owner+0xE0)) != 0` |
| 2 | 4 | Game unique ID | Big endian, equals `DWORD(owner+0xDC)` |
| 6 | 2 | Marker | `SQ` for query, `SR` for response |
| 8 | 8 | Nonce | Big-endian QWORD serialization; response matches `owner+0x4A/+0x4E` |
| 16 | variable | Session data | Present in response, absent from query |

Exact query checks: `0xE40859` length, `0xE40865` version, `0xE40888` platform, `0xE408AE` game ID, `0xE408F2/0xE408F8` marker. The response equivalents are `0xE40944`, `0xE4096F`, `0xE4098F`, `0xE409B5`, `0xE409FD/0xE40A03`; nonce comparison is `0xE40A27/0xE40A36`.

The query builder is `0x00E47490`; the response path is `0x00E50FA0`. Their serializers are byte `0xE419E0`, big-endian DWORD `0x7BD630`, and big-endian QWORD `0xE41A30`. `appGetPlatformType` at `0x4809E0` returns `0x40` when global `0x19F28C8` is set, otherwise `1` or `2` according to global `0x19F28CC`; no live global values were read here.

Installed `DefaultEngine.ini` lines 462–463 contain `LanPacketPlatformMask=0x00000041` and `LanGameUniqueId=0x4DD37BFF`. A conditional example using that default and platform byte `1`, with nonce bytes `01 02 03 04 05 06 07 08`, is:

```text
07 01 4D D3 7B FF 53 51 01 02 03 04 05 06 07 08
```

This is an offline layout example, not a claim that an active PC host replied. Full PC evidence is in `lan-discovery-proof.json`, `IsValidLanQueryPacket.asm` and `IsValidLanResponsePacket.asm`. Two exploratory filenames `lan-header-query-build.asm` and `lan-header-response-build.asm` were assigned before classification; the former contains the `0xE50FA0` **response** path and the latter the `0xE47490` **query** builder. Function addresses and the proof JSON are authoritative.

## LAN beacon uses ordinary UDP

The installed EXE has a complete discovery path around Bifrost:

1. Steam CreateGame branch `0xECE38C..0xECE3AE` reads settings `+0x84` bit 0 and calls base `StartLanBeacon` `0xE45F40` when set.
2. `StartLanBeacon` calls beacon initialization `0xE44C60` with the configured port at owner `+0xD8`.
3. Initialization calls `GSocketSubsystem[0x1AC5F28]` vtable `+8` at `0xE44D3E`.
4. `0xE49D33` selects static subsystem object `0x1AC61C8`; CRT initializer `0x15DEE20` constructs it through `0xE67960`, which sets vtable `0x17BCB78` at `0xE679CE`.
5. That vtable's `+8` entry is `0xE49F50`: it calls `socket(AF_INET=2, SOCK_DGRAM=2, IPPROTO_UDP=17)` and installs socket vtable `0x17B9020`.
6. Socket vtable `+0x24` is raw send wrapper `0xE3EDE0`, calling WSOCK32 `sendto`; `+0x2C` is receive wrapper `0xE3EE70`, calling `recvfrom`. These wrappers forward the caller buffer directly.

The native LAN discovery implementation therefore does not require Bifrost or RakNet framing. Gameplay vtables were also checked: base TcpipConnection vtable `0x17BC780` and Steam connection vtable `0x17C83E0` both use LowLevelSend `0xE40AD0` at slot `+0x154`. It forwards caller bytes to socket `this+0x5064`, vtable `+0x24`, at `0xE40BDA`. Their InitConnection slot `+0x188` differs (`0xE5C7D0` versus `0xEBB0B0`). The packet capture below establishes ordinary UE3 framing on the tested PC gameplay endpoint; config alone was not used to infer that result.

## UE3 connection version gate

The compiled current engine version at `0x019B987C` and minimum network version at `0x019B9884` are both **8639**. Hello sender `0xD1D3E0`, called at `0xD24481`, serializes control ID 0, minimum version, current version, an additional DWORD, and two 16-byte `FDlcCompatibilityData` structures.

Server Hello path `0xAD54D2` receives these through `0xABAA60`. At `0xAD5510` and `0xAD5522` it rejects when:

```text
remoteCurrent < localMin || remoteMin > localCurrent
```

The static PC accepted range is `[8639, 8639]`. Rejection sends Upgrade through `0xAC9150` at `0xAD5662`, then flushes/closes. The pending-connection handler at `0xD242E0` receives Upgrade's two DWORDs through `0xD0B0A0`; it selects `ClientOutdated` at `0xD244CA` or `ServerOutdated` at `0xD244F9`.

After version compatibility, the two `FDlcCompatibilityData` tests can still reject with control IDs 39 and 40 at `0xAD555B` and `0xAD55C3`. These structures must not be conflated with package GUID checks; matching UE3 version numbers alone does not establish playable cross-platform compatibility. See `exe-protocol-proof.json` and the Hello/Upgrade assembly excerpts.

## BuildUniqueId: separate fact, absent from PC LAN header

`UOnlineSubsystem::execGetBuildUniqueId` at `0xD5E0C0` dispatches vtable `+0x148`. The relevant getter `0xCC4E00` returns `this+0xC8` when `this+0xC4` mask `1` is set. Otherwise it obtains the Engine outermost package and calls CRC `0x4779C0` on its GUID at package `+0x5C`, 16 bytes, seed 0.

Engine's GUID is `57CAAACB437226F5C97867A9A28097E6`, with serialized little-endian DWORD bytes:

```text
CB AA CA 57 F5 26 72 43 A9 67 78 C9 E6 97 80 A2
```

CRC table initializer `0x480B40` generates the MSB-first `0x04C11DB7` polynomial table, explicitly XORing the polynomial at `0x480B66`. The table at `0x19F8200` is runtime BSS and cannot be read from PE file bytes. PC's DWORD loop processes bytes low to high, matching the serialized GUID bytes; CRC state shifts left, starts inverted and ends inverted.

Default result is **`0x8F49B216`**, unsigned **2403971606**, signed **-1890995690**. The independent bitwise model also passes `123456789 → 0xFC891918`. Live override flags and values were not inspected. Most importantly, **this computed ID is not compared by the PC version-7 discovery validators above**. Evidence: `build-id-proof.json`, `GetBuildUniqueId.asm`, `appMemCrc.asm`, `appMemCrc-init.asm`.

## Bifrost: separate peer framing

`bifrost_sockets_sendTo` `0x1019B4A0` enters worker `0x101A3E00`, which checks service initialization, active peer and nonzero logical port. It calls peer send `0x1026F630`; its direct established-connection route calls `0x10286D90` at `0x1026FA28`, and its deferred route queues packets. It is not a direct Winsock wrapper.

Event header serializer `0x102815F0` writes a **22-byte** header:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 1 | `kP2PVersion = 4` |
| 1 | 2 | Logical port A, little endian |
| 3 | 2 | Logical port B, little endian |
| 5 | 16 | GUID bytes, zero allowed |
| 21 | 1 | Flags |

The paired reader `0x10281690` rejects a first byte other than 4 at `0x102816B4`; it requires a message of at least 23 bytes. RTC send `0x10286D90` constructs the same header inline at `0x10286E83..0x10286EDA`, appends caller payload through `0x10291AB0` at `0x10286F26`, then invokes the selected channel's vtable `+0x50` at `0x10286F4D`. Receive `0x102828D0` parses the header at `0x10282992`, advances the payload pointer and subtracts header length at `0x10282A23/0x10282A26`, and queues the remaining bytes by ports.

Retained source-path strings identify `sockets/connection.cpp`, `sockets/event_data.cpp`, `sockets/rtc_connection.cpp`; the DLL also contains WebRTC DataChannel/STUN/TURN components. Evidence is the actual framing code, not the strings alone. See `bifrost-framing-proof.json`, `dll-event-header-write.asm`, `dll-event-header-read.asm`, `dll-rtc-frame-send.asm`, `dll-rtc-frame-receive.asm`.

No complete standard RakNet offline magic was found in the three checked PC binaries. This bounded absence does not assign a RakNet version to PC. In particular, Bifrost inner version 4 and Android RakNet offline version 6 are separate layers, and their numeric difference says nothing by itself about the raw LAN discovery path.

## Verification and remaining boundary

`verify_pc_protocol.py` passed pinned EXE/DLL SHA256 checks, precise instruction assertions for the UE3 gate, PC LAN validators, raw socket vtables, Bifrost header, the CRC initializer and a BSS read rejection. It also verifies the default CRC with an independent bitwise implementation and exercises the observed PC query predicate with accepted/rejected offline vectors. No dependencies were downloaded.

This establishes the installed PC's static discovery and UE3 control formats. The supplied gameplay capture below proves bidirectional UDP activity and exposes two earlier wire-format barriers. A playable joined session, mobile adaptation and compatibility of serialized session data remain runtime work for the root agent. Merely editing engine-version constants or BuildIdOverride is not a demonstrated complete LAN fix.

## Actual gameplay capture: three-byte prefix, matching UE3 bit core

The root supplied `direct-relay1.json`, SHA256 `c63901df3b242c64562851a0d584658addd69c33aed691b72c14604be410d64e`. Reading this existing capture remains host-only analysis; this agent did not create its sockets or relay traffic.

PC functions were resolved from the actual connection vtable and instruction flow:

| Function | Preferred PC VA | Concrete behavior |
|---|---|---|
| PreSend / packet-header writer | `0x00D0BD10` | When Out is empty, writes `this+0xF54` with max `0x4000` at `0xD0BD5D` |
| FlushNet | `0x00D20240` | Writes termination bit 1, then zeroes to byte alignment; vtable `+0x164` |
| SendAck | `0x00D0F650` | Writes ACK flag 1, then ACK ID with max `0x4000` |
| ReceivedRawPacket | `0x00D24160` | Excludes final byte's highest-set bit as terminator, then calls ReceivedPacket |
| ReceivedPacket | `0x00D20950` | Reads packet ID with max `0x4000`, then ACK/bunch flag and bounded ACK ID |
| FBitWriter::WriteInt(value,max) | `0x004314D0` | Integer value is passed by value |
| FBitWriter::SerializeInt(DWORD&,max) | `0x004313B0` | Native virtual writer at writer vtable `0x01657988 + 0x0C` |
| FBitReader::ReadInt(max) | `0x0042B500` | Wrapper dispatches native virtual reader |
| FBitReader::SerializeInt(DWORD&,max) | `0x00431960` | Native reader at reader vtable `0x01657A00 + 0x0C` |

Both PC integer routines walk value bits from low to high; byte-mask table `0x019B849C` contains `01 02 04 08 10 20 40 80`. Packet ID and ACK ID therefore each occupy **14 bits**, LSB first. Header-only PC packet sequence 1 is `01 40`: the 14-bit ID is followed by the termination bit at bit 14. `TcpipConnection::InitConnection` at `0xE5C7D0` defaults MaxPacket to `0x300 = 768` bytes at `0xE5C982`; this gives 13 bits for the captured 40-bit bunch-size value.

Initial inspection of phone bytes as a PC packet suggested a 32-bit prefix. The parent's mobile native analysis established that mobile PreSend conditionally inserts **three bytes** before the same UE3 14-bit packet ID. This distinguishes a mobile optional prefix from any fault in the underlying integer bit order.

The offline model was tested against the whole supplied capture:

| Capture direction / interpretation | Packets | Result |
|---|---:|---|
| PC bytes, unmodified | 66 | IDs `0…65`; every packet is ACK-only, all ACK IDs are 0; all 66 reconstruct byte-for-byte |
| Phone bytes, interpreted directly by PC | 134 | Every ID reads as 0; PC starts a bunch at bit 14 |
| Phone bytes after removing exactly three leading bytes | 134 | IDs `0…133`, no parse errors |
| Short phone packets after strip3 | 119 | Valid empty UE3 packets; all reconstruct byte-for-byte |
| Long phone packets after strip3 | 15 | One control-channel bunch each; all reconstruct byte-for-byte with native PC default MaxPacket 768 |

Every one of those 15 bunches has `control=1, open=1, close=0, reliable=1, channelIndex=0, channelSequence=1, channelType=1, dataBits=40`, and payload **`1A 00 02 00 00`**. The first payload begins at bit 55 and ends exactly at bit 95. No Hello payload is present among these 15 retransmissions.

Removing four bytes fails on 119 short packets; reversing each byte's bit order does not fix the format. Those rejected candidate transforms are retained in `phone-candidate-transform-probe.json` to avoid repeating them. The successful complete evidence is `phone-strip3-capture-analysis.json`; executable offline models are in `pc_wire.py`. `game-wire-prefix-proof.json` contains the exact native addresses. Exploratory `*-candidate.asm` filenames predate identification; the final function table above and non-candidate excerpts are authoritative.

## HandshakeStart: PC platform field is one byte

PC registers control ID **26** as **`HandshakeStart`**: literal `0x0177FE80`, name registry `0x01ABEB80 + 26*4 = 0x01ABEBE8`, initialized at `0x015D6B36`.

PC `FNetControlMessage<26>::Send(Connection*, BYTE&)` is `0x005612F0`. It writes control byte 26 at `0x561356/0x56135A`, then calls `Serialize(arg2, 1)` at `0x561368..0x561371`. Callers `0x581C30..0x581C3D` and `0xD2E569..0xD2E57F` obtain `appGetPlatformType()` at `0x4809E0`, store **AL**, and pass a pointer to that byte. This proves the field's platform semantics and its size independently of a product-version string.

The receive side independently confirms the same schema. Control-channel receiver `0xD1F050` dispatches ID 26 at `0xD1F124` to `0xD1F25A`. With connection handshake state `+0x501C == 0`, it creates a random challenge, stores state 1 and sends HandshakeChallenge through `0xD1E3B0`. The handler leaves the incoming reader position unchanged, so the normal Discard fallback consumes the field. The fallback table entry `DWORD[0xD1F604 + 26*4]` is `0xD1F54D`, whose call at `0xD1F55A` reads **one byte**, not a DWORD.

The parent independently resolved the mobile field to `appGetPlatformType() == 0x200`, serialized as a **32-bit value**. Thus the captured `1A 00 02 00 00` is HandshakeStart plus Android's platform bit `0x200`, not MaxPacket 512. PC consumes only the first platform byte and leaves the other three bytes for its message loop. Adapting the three-byte packet prefix alone therefore does not align this following control-message schema. Platform mapping and both peers' handshake behavior must be considered explicitly.

Evidence is in `HandshakeStart-proof.json`, `HandshakeStart-Send.asm`, its two exact caller excerpts, `HandshakeStart-Discard.asm`, and `ControlChannel-handshake-dispatch*.asm`. The root is testing connection changes; no successful joined session or schema adaptation is claimed here.

## Checkpoint at explicit LAN stop

LAN research was explicitly stopped by the parent. No further socket, device, process, source, configuration or package change was performed by this agent. The findings below are a handoff of completed offline work; they do not claim a joined session.

The generic `ue3_packets.py` decoder/encoder preserves ordered ACKs, reliable sequence numbers, control/open/close flags, payload bit counts and the native trailing termination bit. It reproduced every analyzed forwarded datagram byte-for-byte: **85/85** in `direct-relay2-rawdriver.json`, **110/110** in `direct-relay3-raw-handshake26.json`, and the complete relay4 capture. Phone packets use `forward_hex` when the root's experimental relay provided it; PC packets use `hex`. The public `pc_wire.decode_single_bunch` / `encode_single_bunch` APIs remain compatible with the parent's isolated normalizer.

Relay2's PC sends HandshakeChallenge and then closes approximately 70 microseconds later, before the phone's HandshakeResponse. The unmatched mobile DWORD platform field leaves bytes `02 00 00` after PC consumes its one-byte platform; PC interprets byte 02 as Upgrade, whose two-DWORD body underflows. The PC challenge-response calculation itself matches the phone exactly: format `895fcf626f55798667e4e94cb7a636af %d`, signed decimal challenge, MSB polynomial `04C11DB7`, initial/final XOR `FFFFFFFF`, UTF-16LE characters without NUL. Challenge `0x55747182` gives response `0x80CD5C5F`. Evidence: `rawdriver-capture-analysis.json`, `HandshakeResponse-proof.json`, native excerpts and the hash-pinned verifier.

Relay3's single matched HandshakeStart transformation produces the 16-bit body `1A 40`. The subsequent challenge/response succeeds; PC sends HandshakeComplete 29. The phone then sends a **45-byte Hello** with min=current **8636**, additional DWORD 0, and two zero-valued 16-byte **FDlcCompatibilityData** structures. PC answers Upgrade body `02 BF210000 BF210000`, requiring min=current **8639**, then closes. The root observed ClientOutdated. Evidence: `handshake26-normalized-capture-analysis.json`.

Relay4 explicitly also changes only the experimental forwarded Hello version DWORDs to 8639. Its next actual rejection is **NMT39 `DlcCompatFailure`**, body `27 00000000 00000000 00000000 7FABBF00`, followed by close 42.5 microseconds later. Its FDlcCompatibilityData DWORDs are `(0, 0, 0, 0x00BFAB7F)`. This is before package-map acceptance or a joined session. PC name literal `0x0178000C`, name registry `0x01ABEC1C`, initializer `0x015D6E76`; NMT40 is `DlcMapVerifyServer` at literal `0x01780030`. The native Hello gate XORs/ORs all four received connection words `+0x150..+0x15C` against local data at `0xAD555B..0xAD5585`, then sends39 through `0xAC9800` at `0xAD558C`. The source object comes from getter `0x5881B0` and virtual slot `+0x1C4`; it was **not proven to be OnlineSubsystem**. Evidence: `experimental-hello8639-capture-analysis.json`, `FDlcServerGate.asm`, `FDlcHello-Receive.asm`, `FDlcMismatch-Send39.asm`.

`extract_script_schema.py` now normalizes the genuine PC/mobile layout differences: PC-only UProperty Category/ArrayEnum, PC-only UStruct editor fields, and PC-only UFunction operator precedence/friendly name. It parses native UFunction prefix WORD arrays, NetIndex, flags, superclass/children references, RPC parameters, and property flags/array dimensions/typed references. Attribute property tails contain two object references (Float/Int) or three (Byte), following the parent's mobile native serialization proof. The authoritative PC inputs are `schema-pc-{Core,Engine,GameFramework,GearboxFramework,WillowGame}-v3.json`; parent-owned mobile inputs are `../mobile-schemas-v3/schema-mobile-*.json`. These inputs have zero property/function parse errors and zero unexplained trailing bytes in those parsers. Raw script-body hashes are evidence only; editor stripping means a raw hash mismatch is not a compatibility conclusion.

`script-package-flags-proof.json` hashes all ten package inputs and records actual cooked flags. Core/Engine/GameFramework are PC `E4A80008` / mobile `A4A80008`; GearboxFramework/WillowGame are `E4A80009` / `A4A80009`. Every package has bit8 set. This satisfies the parent's mobile SerializeNetIndex guard for preserving serialized source NetIndex.

`network-schema-comparison-five-packages.json`, generated by `compare_network_schemas.py`, covers **112** classes with own NetFields. Under the parent's proven mobile ascending-NetIndex sort model, **14** own field orders differ. Engine.Actor has the same 24 field paths but a different order: rank1 is PC `bBlockActors`, mobile `RelativeRotation`. WillowPlayerController has 303/298 own fields and WillowPlayerPawn 27/19. The controller has 14 PC-only paths (13 RPCs plus `MeleeSelfChance`) and nine mobile-only RPCs; the pawn has eight PC-only paths (five RPCs plus three replicated properties). The JSON gives complete ordered lists, source NetIndex, normalized schemas, superclass counts and zero-based inherited model rank mappings. Player inheritance is resolved after including GearboxFramework; only PC-only Buzzaxe classes remain absent on mobile. **These are metadata model ranks, not independently proved PC runtime wire indices.**

Eight shared schemas differ under strict comparison: six UFunction flag changes and two parameter-name capitalization changes. The shared ordered RPC parameter classes, array dimensions, flags and normalized reference paths match at the current shallow reference level. Exact flag XORs/parameters can be read from the JSON. Recursive equality of referenced ScriptStruct members, Enum values and Array inner schemas has not been completed. Neither a GUID alias nor a production version change was made; a field mapper can be researched using shared paths and checked schemas, failing closed on missing fields.

The bounded raw-immediate scan `network-version-broad-immediate-scan.json` found no confirmed code comparison against DWORD thresholds 8600..8640; its matches are not full control-flow proof and do not establish version compatibility. PC ArNetVer-dependent serializers, PC native ClassNetCache ABI/order, recursive type schemas, and an actual joined session remain unresolved.

No native `NoDLC` or `DisableDLC` UTF-16/ASCII literal was found in the installed EXE. Installed `DefaultDLCManager.ini` has OnlineCompatibilityInfo and DLC/offer lists but this alone proves no stock disable switch. Work toward a base-content directory view stopped before proving the enumeration base-directory formula. `dlc-path-string-candidates.json` contains bounded string/xref candidates only. **Exploratory file warnings:** `dlc-directory-builder.asm` actually shows the DLC console-command dispatcher, not a proved directory builder; `GetDlcCompatibilityData.asm` came from an unverified vtable candidate and must not be treated as the Hello gate's compatibility getter. The authoritative gate proof is the explicit instruction flow above.


---

## SHA-256 включённых источников

```json
[
  {
    "path": "work/evidence/lan-pc-android-20261019/HOST_RESEARCH.md",
    "bytes": 12555,
    "sha256": "5e2c01ee9fb5e696ec0f311a081edde744f95159b439b4adee1d1f4b6970e70c"
  },
  {
    "path": "work/evidence/lan-pc-android-20261019/DUAL_DISCOVERY_SCOPE.md",
    "bytes": 10473,
    "sha256": "39088c80eaa425c04cab2511d837b6264a00a6c725771de5e79ed2fedee2bcb3"
  },
  {
    "path": "work/evidence/lan-pc-android-20261019/QA597_RELRO_GUARD.md",
    "bytes": 3267,
    "sha256": "46f4a7eef5b9ec8494af732c03da2d0de6e0a068957e3aa471aa05508e4e58e4"
  },
  {
    "path": "work/evidence/lan-pc-android-20261019/protocol-compat/PROTOCOL.md",
    "bytes": 16170,
    "sha256": "114812b874329ff63be727ff4f8f179883089d051d6c75d69dda3d08da6523c3"
  },
  {
    "path": "work/evidence/lan-pc-android-20261019/protocol-compat/LAN_HANDOFF_STOPPED.md",
    "bytes": 7498,
    "sha256": "3dec248376ceb817117b2c9e17d701d9ab82354909018e3b96df3fcd5750a82a"
  },
  {
    "path": "work/evidence/lan-pc-android-20261019/protocol-compat/pc/RESULT.md",
    "bytes": 22736,
    "sha256": "00d8e070066bcb04f44ac19ec209c9e0452eb873be93ac2c264f37ece449f281"
  }
]
```
