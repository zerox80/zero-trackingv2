# pagy blocker

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) A Chrome browser extension demonstrating an ad blocker built with Manifest V3, focusing on performance using the Declarative Net Request API.

## Overview

This extension provides basic ad and tracker blocking functionality by parsing a filter list (similar to EasyList format) and converting the rules into the format required by Chrome's `declarativeNetRequest` API. It prioritizes efficiency and adherence to the Manifest V3 architecture.

**Blocking runs in the browser kernel, not in JavaScript**, as all filter rules are implemented directly in the native browser engine via the `declarativeNetRequest` API.
## Features

* **Manifest V3 Compliant:** Built using the modern Chrome extension platform.
* **Declarative Net Request API:** Leverages Chrome's efficient, privacy-preserving mechanism for blocking network requests (Blocking in Browser‑Kernel, not in JavaScript).
* **Filter List Parsing:** Reads rules from `filter_lists/filter.txt`.
    * Supports common filter syntax like `||domain.example^`.
    * Supports `$domain=` options for specifying triggering/excluded domains.
* **Dynamic Rule Updates:** Efficiently loads and updates blocking rules.
    * Removes existing rules before adding new ones.
    * Adds rules in batches to avoid hitting API limits.
    * Handles rule count limits (`MAX_NUMBER_OF_DYNAMIC_AND_SESSION_RULES`).
* **Simple Popup UI:** Provides a basic popup to view the number of currently loaded rules and manually refresh them.
* **Background Service Worker:** Manages rule loading, updates, and browser events.
* **Error Handling:** Displays basic error states (e.g., 'ERR' on the extension icon badge) if initialization or rule updates fail.

## Technology Stack

* **Manifest V3**
* **JavaScript** (ES Modules in Service Worker)
* **Chrome Extension APIs:**
    * `declarativeNetRequest`
    * `storage`
    * `action`
    * `runtime`
* **HTML / CSS** (for the popup)
* **(Experimental) C++ / WebAssembly (WASM)**: via Emscripten for the alternative parser (`wasm/parser.cc`, `wasm/filter_parser.js`). *Note: The WASM parser is not currently integrated into the main background script.*

## Installation (Development)

1.  **Clone or Download:** Get a copy of this repository.
    ```bash
    git clone https://github.com/zerox80/zero-trackingv2.git
    cd zero-trackingv2
    ```
2.  **Open Chrome/Chromium Extensions:** Navigate to `chrome://extensions` in your browser.
3.  **Enable Developer Mode:** Toggle the "Developer mode" switch, usually in the top-right corner.
4.  **Load Unpacked:** Click the "Load unpacked" button.
5.  **Select Directory:** Choose the `adblock-extension` folder (the one containing `manifest.json`).

The extension icon should appear in your browser toolbar.

## Usage

* The extension automatically loads rules from `filter_lists/filter.txt` when installed or when the browser starts.
* It blocks network requests matching the loaded rules across all websites (`<all_urls>`).
* Click the extension icon in the toolbar to open the popup:
    * It displays the number of currently active blocking rules.
    * Click the "Aktualisieren" (Refresh) button to manually trigger a reload of the filter list and update the rules. *(Consider changing button text to English if desired)*
* The extension badge (the small text on the icon) will:
    * Be empty upon successful initialization/update.
    * Show 'ERR' or 'UPD ERR' in red if a critical error occurs during setup or rule updates.

## compiling with emcc
emcc parser.cc -o filter_parser.js -std=c++20 -O3 -I . --bind -s WASM=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -sWASM_BIGINT -sNO_DYNAMIC_EXECUTION=1
## Future Improvements / Roadmap

* Integrate the WebAssembly parser for potentially faster filter list processing.
* Expand support for more complex filter list syntax (e.g., element hiding, advanced options).
* Implement more robust statistics gathering (blocked request counts are limited by DNR feedback granularity).
* Add user options (e.g., enabling/disabling lists, custom rules).
* Implement automated filter list updates.

## Contributing

Contributions are welcome! Please feel free to submit a pull request or open an issue.

## License

This project is licensed under the MIT License - see the `LICENSE.md` file for details.

