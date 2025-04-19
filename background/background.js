import { updateRules } from '../js/rule_parser.js';
import createFilterParserModule from '../wasm/filter_parser.js';

let wasmModule = null;

async function clearBadge() {
    try {
        if (chrome.action && chrome.action.setBadgeText) {
            await chrome.action.setBadgeText({ text: '' });
        }
    } catch (error) { }
}
async function setErrorBadge(text = 'ERR') {
     try {
        if (chrome.action && chrome.action.setBadgeText && chrome.action.setBadgeBackgroundColor) {
            await chrome.action.setBadgeText({ text });
            await chrome.action.setBadgeBackgroundColor({ color: '#FF0000' });
        }
     } catch (error) { console.warn("Could not set error badge:", error.message); }
}

async function ensureWasmModuleLoaded() {
    if (!wasmModule) {
        console.log("Initializing WASM module instance...");
        console.time("WASM Module Init");
        try {
            wasmModule = await createFilterParserModule();
            console.timeEnd("WASM Module Init");
            console.log("WASM module instance initialized.");

            if (!wasmModule || typeof wasmModule.parseFilterListWasm !== 'function') {
                 wasmModule = null;
                 throw new Error("WASM module loaded, but 'parseFilterListWasm' function not found or not a function.");
            }
        } catch(error) {
            console.error("Failed to load or initialize WASM module:", error);
            if (error.stack) { console.error(error.stack); }
            wasmModule = null;
             throw error;
        }
    } else {
         console.log("Using existing WASM module instance.");
    }
    return wasmModule;
}

async function fetchFilterList() {
    const filterListURL = chrome.runtime.getURL('filter_lists/filter.txt');
    console.log(`Workspaceing filter list from URL: ${filterListURL}`);

    const response = await fetch(filterListURL);
    if (!response.ok) {
      throw new Error(`Failed to fetch filter list: ${response.statusText} (Status: ${response.status}, URL: ${response.url})`);
    }
    const filterListText = await response.text();
    console.log(`Workspaceed filter list (${filterListText.length} chars).`);
    return filterListText;
}

function parseListWithWasm(module, filterListText) {
    console.time("WASM Parsing");
    const resultJsonString = module.parseFilterListWasm(filterListText);
    console.timeEnd("WASM Parsing");

    if (!resultJsonString) {
        throw new Error("WASM parser returned empty result.");
    }
    let result;
    try {
        result = JSON.parse(resultJsonString);
    } catch (parseError) {
        console.error("Failed to parse JSON result from WASM:", resultJsonString);
        throw new Error(`Failed to parse JSON result from WASM: ${parseError.message}`);
    }

    if (!result || !Array.isArray(result.rules) || typeof result.stats !== 'object') {
         console.error("Invalid result structure from WASM parser:", result);
         throw new Error("Invalid result structure from WASM parser.");
    }
    console.log(`WASM Parser Stats: Total lines: ${result.stats.totalLines}, Processed rules: ${result.stats.processedRules}, Skipped lines: ${result.stats.skippedLines}`);
    console.log(`Parsed ${result.rules.length} rules via WASM.`);
    return result;
}

async function initialize() {
  console.log("Initializing AdBlocker (using WASM Parser)...");
  await clearBadge();
  try {
    const currentWasmModule = await ensureWasmModuleLoaded();
    const filterListText = await fetchFilterList();
    const { rules } = parseListWithWasm(currentWasmModule, filterListText);

    console.log(`Attempting to add ${rules.length} rules.`);
    console.log("First few rule IDs:", rules.slice(0, 20).map(r => r.id));

    await updateRules(rules);

    const badgeTextAfterUpdate = await chrome.action.getBadgeText ? await chrome.action.getBadgeText({}) : '';
    if (!badgeTextAfterUpdate || !badgeTextAfterUpdate.includes('ERR')) {
        await clearBadge();
        console.log("Initialization complete (WASM).");
    } else {
        console.log("Initialization completed but updateRules set an error badge.");
    }

  } catch (error) {
    console.error("Initialization failed (WASM):", error);
    if (error.stack) { console.error(error.stack); }
    await setErrorBadge('INIT ERR');
    wasmModule = null;
  }
}


chrome.runtime.onInstalled.addListener(details => {
  console.log("Extension installed or updated:", details.reason);
  initialize();
});

chrome.runtime.onStartup.addListener(async () => {
    console.log("Browser startup detected.");
    const currentBadge = await chrome.action.getBadgeText ? await chrome.action.getBadgeText({}) : '';
    if (currentBadge && currentBadge.includes('ERR')) {
         console.log("Attempting re-initialization on startup due to previous error state.");
         initialize();
    } else {
         await clearBadge();
         if (!wasmModule) {
             console.log("Pre-initializing WASM module instance on startup.");
             ensureWasmModuleLoaded()
                .then(async () => {
                    console.log("WASM module pre-initialized successfully on startup.");
                    const badgeAfterPreInit = await chrome.action.getBadgeText ? await chrome.action.getBadgeText({}) : '';
                    if (!badgeAfterPreInit || !badgeAfterPreInit.includes('ERR')) {
                    }
                 })
                .catch(async (err) => {
                    console.error("Background WASM pre-initialization failed:", err);
                    const badgeBeforeSettingPreErr = await chrome.action.getBadgeText ? await chrome.action.getBadgeText({}) : '';
                    if (!badgeBeforeSettingPreErr || !badgeBeforeSettingPreErr.includes('ERR')) {
                       await setErrorBadge('PRE ERR');
                    } else {
                        console.warn("Background WASM pre-initialization failed, but an error state already exists.");
                    }
                 });
         }
    }
});

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "getStats") {
    chrome.storage.local.get(['ruleCount']).then(storageData => {
        sendResponse({ ruleCount: storageData.ruleCount === undefined ? 'N/A' : storageData.ruleCount });
    }).catch(error => {
        console.error("Error getting stats from storage:", error);
        sendResponse({ ruleCount: 'Fehler' });
    });
    return true;
  }

  if (request.action === "reloadRules") {
      console.log("Reloading rules triggered by popup message...");
      initialize().then(async () => {
          await new Promise(resolve => setTimeout(resolve, 200));
          const storageData = await chrome.storage.local.get(['ruleCount']);
          sendResponse({
              success: true,
              message: "Rules reloaded using WASM parser.",
              ruleCount: storageData.ruleCount === undefined ? 'N/A' : storageData.ruleCount
          });
      }).catch(error => {
          console.error("Failed to reload rules via message:", error);
          sendResponse({ success: false, message: `Failed to reload rules: ${error.message}` });
      });
      return true;
  }

  return false;
});

initialize();