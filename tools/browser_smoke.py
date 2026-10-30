#!/usr/bin/env python3
"""Load the exported Godot Web build in a real browser and wait for markers.

With ?smoke=1 in the URL the demo opens its test harness, which prints
JSFX_WEB_READY after setup and JSFX_WEB_PROCESSED once the JSFX effect has
processed audio frames. With --playground the script opens the playable demo
instead, clicks "start audio", holds a key and waits for the synth to sound. Requires:

    python -m pip install playwright

This uses an installed Chrome/Edge via executable_path, so no browser download
is required.

Usage:
    python tools/browser_smoke.py --url "http://127.0.0.1:8060/index.html?smoke=1"
    python tools/browser_smoke.py --url "http://127.0.0.1:8061/index.html?smoke=1" --threads
    python tools/browser_smoke.py --url http://127.0.0.1:8060/index.html --playground
"""

import argparse
import sys
import time

from playwright.sync_api import sync_playwright


def wait_for_log(page, logs, marker, timeout_ms, click_canvas=False):
    deadline = time.time() + timeout_ms / 1000.0
    while time.time() < deadline:
        for line in logs:
            if marker in line:
                return line
        if click_canvas:
            try:
                page.locator("canvas").click(
                    position={"x": 200, "y": 150}, timeout=1000)
            except Exception:
                pass
            try:
                page.keyboard.press("Space")
            except Exception:
                pass
        page.wait_for_timeout(500)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--threads", action="store_true",
                        help="Require cross-origin isolation (threaded build)")
    parser.add_argument("--headed", action="store_true")
    parser.add_argument("--timeout-ms", type=int, default=90000)
    parser.add_argument("--executable", default=None,
                        help="Chrome/Edge executable; defaults are tried")
    parser.add_argument("--screenshot", default="")
    parser.add_argument("--playground", action="store_true",
                        help="Play a note in the playable demo instead")
    parser.add_argument("--settle-ms", type=int, default=0,
                        help="Keep audio running this long before measuring")
    args = parser.parse_args()

    candidates = [
        args.executable,
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    ]
    candidates = [c for c in candidates if c]

    launch_args = [
        "--autoplay-policy=no-user-gesture-required",
        "--enable-unsafe-swiftshader",
        "--enable-features=SharedArrayBuffer",
    ]

    logs = []
    page_errors = []

    with sync_playwright() as playwright:
        browser = None
        last_error = None
        for executable in candidates:
            try:
                browser = playwright.chromium.launch(
                    executable_path=executable,
                    headless=not args.headed,
                    args=launch_args,
                )
                break
            except Exception as exc:  # noqa: BLE001 - report launch failures
                last_error = exc

        if browser is None:
            print(f"BROWSER_LAUNCH_FAILED: {last_error}")
            return 2

        context = browser.new_context(viewport={"width": 800, "height": 600})
        page = context.new_page()
        page.add_init_script("""
            // Track every WebAssembly memory so the heap size can be reported.
            window.__jsfxMemories = [];
            const OriginalMemory = WebAssembly.Memory;
            WebAssembly.Memory = function(descriptor) {
                const memory = new OriginalMemory(descriptor);
                window.__jsfxMemories.push(memory);
                return memory;
            };
            WebAssembly.Memory.prototype = OriginalMemory.prototype;
            for (const name of ["instantiate", "instantiateStreaming"]) {
                const original = WebAssembly[name];
                WebAssembly[name] = async function(...args) {
                    const result = await original.apply(this, args);
                    const exports = (result.instance || result).exports || {};
                    for (const value of Object.values(exports)) {
                        if (value instanceof OriginalMemory) {
                            window.__jsfxMemories.push(value);
                        }
                    }
                    return result;
                };
            }
            window.__jsfxAudioContexts = [];
            const OriginalAudioContext = window.AudioContext;
            if (OriginalAudioContext) {
                window.AudioContext = function(...args) {
                    const ctx = new OriginalAudioContext(...args);
                    window.__jsfxAudioContexts.push(ctx);
                    return ctx;
                };
                window.AudioContext.prototype = OriginalAudioContext.prototype;
            }
        """)
        page.on("console", lambda message: logs.append(
            f"[{message.type}] {message.text}"))
        page.on("pageerror", lambda error: page_errors.append(str(error)))

        page.goto(args.url, wait_until="domcontentloaded", timeout=60000)

        try:
            page.wait_for_selector("canvas", timeout=30000)
        except Exception:
            pass

        # Give the canvas a focus/activation click, in case Web Audio needs it.
        try:
            page.locator("canvas").click(
                position={"x": 200, "y": 150}, timeout=5000)
        except Exception:
            pass

        if args.playground:
            ready = wait_for_log(page, logs, "JSFX_PLAYGROUND_READY",
                                 args.timeout_ms)
            page.wait_for_timeout(500)
            # The centre of the canvas is the "start audio" button; then hold
            # A (C3) on the computer keyboard.
            box = page.locator("canvas").bounding_box()
            page.mouse.click(box["x"] + box["width"] / 2,
                             box["y"] + box["height"] / 2)
            page.wait_for_timeout(300)
            page.keyboard.down("a")
            processed = wait_for_log(page, logs, "JSFX_PLAYGROUND_SOUND",
                                     args.timeout_ms)
            if args.screenshot:
                page.screenshot(path=args.screenshot)
            page.keyboard.up("a")
        else:
            ready = wait_for_log(page, logs, "JSFX_WEB_READY", args.timeout_ms)
            # A click after startup is what resumes a suspended Web Audio
            # context in Godot's Web driver.
            processed = wait_for_log(page, logs, "JSFX_WEB_PROCESSED=",
                                     args.timeout_ms, click_canvas=True)

        if args.settle_ms > 0:
            page.wait_for_timeout(args.settle_ms)

        try:
            heap_mb = page.evaluate(
                "Math.max(0, ...(window.__jsfxMemories || []).map("
                "m => m.buffer.byteLength)) / 1048576")
        except Exception:
            heap_mb = None

        try:
            isolated = page.evaluate("crossOriginIsolated")
        except Exception:
            isolated = None

        try:
            audio_states = page.evaluate(
                "(window.__jsfxAudioContexts || []).map(c => c.state)")
        except Exception:
            audio_states = None
        try:
            user_active = page.evaluate(
                "navigator.userActivation ? navigator.userActivation.isActive : null")
        except Exception:
            user_active = None

        print(f"WASM_HEAP_MB={heap_mb}")
        print(f"crossOriginIsolated={isolated}")
        print(f"audio_context_states={audio_states}")
        print(f"user_activation={user_active}")
        print(f"JSFX_WEB_READY={'yes' if ready else 'no'}")
        print(f"JSFX_WEB_PROCESSED={'yes' if processed else 'no'}")
        if processed:
            print(f"PROCESSED_LINE={processed}")

        if args.screenshot and not args.playground:
            try:
                page.screenshot(path=args.screenshot)
                print(f"SCREENSHOT={args.screenshot}")
            except Exception as exc:
                print(f"SCREENSHOT_FAILED={exc}")

        for line in logs:
            print(f"LOG {line}")
        for error in page_errors:
            print(f"PAGEERROR {error}")

        failed = False
        if args.threads and not isolated:
            print("ERROR: threaded build is not cross-origin isolated")
            failed = True
        if ready is None:
            print("ERROR: JSFX_WEB_READY was not printed")
            failed = True
        if processed is None:
            print("ERROR: JSFX_WEB_PROCESSED was not printed")
            failed = True

        context.close()
        browser.close()

    if failed:
        print("BROWSER_SMOKE_FAIL")
        return 1

    print("BROWSER_SMOKE_OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
