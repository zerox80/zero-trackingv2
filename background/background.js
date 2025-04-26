// background/background.js

import { updateRules } from '../js/rule_parser.js';
import createFilterParserModule from '../wasm/filter_parser.js';

let wasmInitPromise = null;

/**
 * Lädt das WASM-Modul nur einmal (Singleton-Promise).
 * Bei einem Fehler setzt es die Promise zurück, sodass ein erneuter Versuch möglich ist.
 */
function ensureWasmModuleLoaded() {
  if (!wasmInitPromise) {
    console.log("Initializing WASM module instance...");
    console.time("WASM Module Init");

    wasmInitPromise = createFilterParserModule()
      .then(module => {
        console.timeEnd("WASM Module Init");
        console.log("WASM module instance initialized.");
        if (typeof module.parseFilterListWasm !== 'function') {
          throw new Error("parseFilterListWasm not found or not a function.");
        }
        return module;
      })
      .catch(error => {
        console.error("Failed to load or initialize WASM module:", error);
        if (error.stack) console.error(error.stack);
        // Rücksetzen, damit ein späterer Aufruf neu versucht
        wasmInitPromise = null;
        throw error;
      });
  } else {
    console.log("Using existing WASM module promise.");
  }
  return wasmInitPromise;
}

async function clearBadge() {
  try {
    if (chrome.action?.setBadgeText) {
      await chrome.action.setBadgeText({ text: '' });
    }
  } catch (e) { /* ignore */ }
}

async function setErrorBadge(text = 'ERR') {
  try {
    if (chrome.action?.setBadgeText && chrome.action?.setBadgeBackgroundColor) {
      await chrome.action.setBadgeText({ text });
      await chrome.action.setBadgeBackgroundColor({ color: '#FF0000' });
    }
  } catch (e) {
    console.warn("Could not set error badge:", e.message);
  }
}

async function fetchFilterList() {
  const url = chrome.runtime.getURL('filter_lists/filter.txt');
  console.log(`Fetching filter list from ${url}`);
  const resp = await fetch(url);
  if (!resp.ok) {
    throw new Error(`Fetch failed: ${resp.status} ${resp.statusText}`);
  }
  const text = await resp.text();
  console.log(`Fetched filter list (${text.length} chars).`);
  return text;
}

function parseListWithWasm(module, filterListText) {
  console.time("WASM Parsing");
  const jsonString = module.parseFilterListWasm(filterListText);
  console.timeEnd("WASM Parsing");

  if (!jsonString) {
    throw new Error("WASM parser returned empty string.");
  }

  let result;
  try {
    result = JSON.parse(jsonString);
  } catch (e) {
    console.error("Invalid JSON from WASM:", jsonString);
    throw new Error(`JSON parse error: ${e.message}`);
  }

  if (!Array.isArray(result.rules) || typeof result.stats !== 'object') {
    console.error("Unexpected structure:", result);
    throw new Error("Invalid structure from WASM parser.");
  }

  console.log(
    `Parsed ${result.rules.length} rules. Stats: ` +
    `totalLines=${result.stats.totalLines}, ` +
    `processed=${result.stats.processedRules}, ` +
    `skipped=${result.stats.skippedLines}`
  );
  return result.rules;
}

async function initialize() {
  console.log("Initializing AdBlocker (WASM)...");
  await clearBadge();

  try {
    const wasmModule = await ensureWasmModuleLoaded();
    const listText   = await fetchFilterList();
    const rules      = parseListWithWasm(wasmModule, listText);

    console.log(`Adding ${rules.length} rules…`);
    await updateRules(rules);

    await clearBadge();
    console.log("Initialization complete.");
  } catch (error) {
    console.error("Initialization failed:", error);
    await setErrorBadge('INIT ERR');
  }
}

// === Event-Listener ===
chrome.runtime.onInstalled.addListener(() => {
  console.log("Extension installed/updated.");
  initialize();
});

chrome.runtime.onStartup.addListener(() => {
  console.log("Browser startup detected.");
  initialize();
});

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "getStats") {
    chrome.storage.local.get(['ruleCount'])
      .then(data => sendResponse({ ruleCount: data.ruleCount ?? 'N/A' }))
      .catch(err => {
        console.error("Error getStats:", err);
        sendResponse({ ruleCount: 'Fehler' });
      });
    return true;  // sendResponse wird asynchron verwendet
  }

  if (request.action === "reloadRules") {
    console.log("Reloading rules via popup…");
    initialize()
      .then(async () => {
        // Kleine Verzögerung, bis storage aktualisiert ist
        await new Promise(r => setTimeout(r, 200));
        const data = await chrome.storage.local.get(['ruleCount']);
        sendResponse({
          success: true,
          message: "Rules reloaded.",
          ruleCount: data.ruleCount ?? 'N/A'
        });
      })
      .catch(err => {
        console.error("reloadRules failed:", err);
        sendResponse({ success: false, message: err.message });
      });
    return true;
  }

  return false;
});

// Direkt beim Laden starten
initialize();