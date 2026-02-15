var Dexcom = require('./dexcom');
var Clay = require('pebble-clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

var appSettings = {};
var MMOL_CONVERSION_FACTOR = 18.0182;
var MAX_READINGS_PER_CHUNK = 316;
var MAX_READINGS = 36;
var CACHE_KEY = 'glucose_cache';
var CACHE_DURATION = 10800; /* 3 hours in seconds */

/**
 * Load settings from local storage
 */
function getSettings() {
    try {
        return JSON.parse(window.localStorage.getItem('clay-settings')) || {};
    } catch (e) {
        console.error('Error parsing settings: ' + e.message);
        return {};
    }
}

/**
 * Convert mg/dL to mmol/L if needed
 */
function convertBGValue(value, units) {
    if (units === 'mmol/L') {
        return Math.round((value / MMOL_CONVERSION_FACTOR) * 10);
    }
    return Math.round(value * 10);
}

/**
 * Load glucose cache from localStorage
 */
function loadCache() {
    try {
        var raw = window.localStorage.getItem(CACHE_KEY);
        if (!raw) return [];
        var parsed = JSON.parse(raw);
        if (!Array.isArray(parsed)) return [];
        return parsed;
    } catch (e) {
        console.error('Error loading cache: ' + e.message);
        return [];
    }
}

/**
 * Save glucose cache to localStorage
 */
function saveCache(cache) {
    try {
        window.localStorage.setItem(CACHE_KEY, JSON.stringify(cache));
    } catch (e) {
        console.error('Error saving cache: ' + e.message);
    }
}

/**
 * Merge new readings into cache, deduplicate by timestamp, sort descending, truncate
 */
function mergeCache(cache, newReadings) {
    var byTimestamp = {};
    var i;

    /* Index existing cache entries by timestamp */
    for (i = 0; i < cache.length; i++) {
        byTimestamp[cache[i].t] = cache[i];
    }

    /* Add/overwrite with new readings */
    for (i = 0; i < newReadings.length; i++) {
        var r = newReadings[i];
        byTimestamp[r.t] = r;
    }

    /* Collect into array */
    var merged = [];
    for (var key in byTimestamp) {
        if (byTimestamp.hasOwnProperty(key)) {
            merged.push(byTimestamp[key]);
        }
    }

    /* Sort descending by timestamp */
    merged.sort(function(a, b) { return b.t - a.t; });

    /* Truncate: remove entries older than 3 hours */
    var cutoff = Math.floor(Date.now() / 1000) - CACHE_DURATION;
    var trimmed = [];
    for (i = 0; i < merged.length; i++) {
        if (merged[i].t >= cutoff) {
            trimmed.push(merged[i]);
        }
    }

    return trimmed;
}

/**
 * Encode readings into a byte array for bulk transfer.
 * Each reading is 6 bytes: int16 value (LE) + int32 timestamp (LE).
 */
function encodeReadingsToBytes(values, timestamps) {
    var bytes = [];
    for (var i = 0; i < values.length; i++) {
        var value = values[i];
        var ts = timestamps[i];
        /* int16 little-endian (handle negative via & 0xFFFF) */
        var v = value & 0xFFFF;
        bytes.push(v & 0xFF);
        bytes.push((v >> 8) & 0xFF);
        /* int32 little-endian */
        bytes.push(ts & 0xFF);
        bytes.push((ts >> 8) & 0xFF);
        bytes.push((ts >> 16) & 0xFF);
        bytes.push((ts >> 24) & 0xFF);
    }
    return bytes;
}

/**
 * Send glucose data array to watch using bulk byte array transfer
 */
function sendGlucoseData(cache) {
    if (!cache || cache.length === 0) {
        console.log('No readings to send');
        return;
    }

    var bgUnits = appSettings.BG_UNITS || 'mg/dL';
    var count = Math.min(cache.length, MAX_READINGS);

    /* Prepare arrays for values and timestamps */
    var values = [];
    var timestamps = [];

    for (var i = 0; i < count; i++) {
        values.push(convertBGValue(cache[i].v, bgUnits));
        timestamps.push(cache[i].t);
    }

    console.log('Sending ' + count + ' readings to watch');
    console.log('First reading: ' + values[0] + ' at ' + new Date(timestamps[0] * 1000));
    console.log('Last reading: ' + values[count - 1] + ' at ' + new Date(timestamps[count - 1] * 1000));

    /* Send header first */
    Pebble.sendAppMessage({
        'BG_COUNT': count,
        'BG_UNITS': bgUnits
    }, function() {
        console.log('Sent BG count: ' + count);
        /* Send chunks after header ACK */
        sendChunks(values, timestamps, 0, 0);
    }, function(e) {
        console.error('Failed to send BG count: ' + (e && e.error ? e.error.message : 'unknown'));
    });
}

/**
 * Send readings in chunks via byte array
 */
function sendChunks(values, timestamps, startIndex, retries) {
    if (startIndex >= values.length) {
        console.log('All data sent successfully');
        return;
    }

    var remaining = values.length - startIndex;
    var chunkSize = Math.min(remaining, MAX_READINGS_PER_CHUNK);

    var chunkValues = values.slice(startIndex, startIndex + chunkSize);
    var chunkTimestamps = timestamps.slice(startIndex, startIndex + chunkSize);
    var bytes = encodeReadingsToBytes(chunkValues, chunkTimestamps);

    var msg = {
        'BG_CHUNK': bytes,
        'BG_INDEX': startIndex
    };

    Pebble.sendAppMessage(msg, function() {
        console.log('Sent chunk at index ' + startIndex + ', size ' + chunkSize);
        /* Send next chunk */
        sendChunks(values, timestamps, startIndex + chunkSize, 0);
    }, function(e) {
        console.error('Failed to send chunk at index ' + startIndex + ': ' + (e && e.error ? e.error.message : 'unknown'));
        if (retries < 3) {
            setTimeout(function() {
                sendChunks(values, timestamps, startIndex, retries + 1);
            }, 500);
        } else {
            console.error('Max retries reached for chunk at index ' + startIndex);
        }
    });
}

/**
 * Fetch glucose readings from Dexcom
 */
function fetchGlucoseData() {
    console.log('Fetching glucose data...');

    if (!appSettings.DEX_LOGIN || !appSettings.DEX_PASSWORD) {
        console.error('No Dexcom credentials configured');
        return;
    }

    var accountId = window.localStorage.getItem('dexcom_account_id');
    var sessionId = window.localStorage.getItem('dexcom_session_id');

    var cache = loadCache();

    var dex = new Dexcom(
        appSettings.DEX_LOGIN,
        appSettings.DEX_PASSWORD,
        function(readings) {
            console.log('Received ' + readings.length + ' readings from Dexcom');

            /* Cache session IDs */
            window.localStorage.setItem('dexcom_account_id', dex.accountId);
            window.localStorage.setItem('dexcom_session_id', dex.sessionId);

            /* Convert Dexcom readings to cache format {v, t} */
            var newEntries = [];
            for (var i = 0; i < readings.length; i++) {
                newEntries.push({
                    v: readings[i]._value,
                    t: Math.floor(readings[i]._datetime.getTime() / 1000)
                });
            }

            /* Merge into cache */
            cache = mergeCache(cache, newEntries);
            saveCache(cache);

            /* Send to watch */
            sendGlucoseData(cache);
        },
        appSettings.DEX_REGION || 'ous'
    );

    /* Restore session if available */
    if (accountId && sessionId) {
        dex.accountId = accountId;
        dex.sessionId = sessionId;
    }

    try {
        if (cache.length > 0) {
            /* Incremental fetch: only get new readings since newest cached */
            var minutesSinceNewest = Math.ceil((Date.now() / 1000 - cache[0].t) / 60);
            if (minutesSinceNewest < 10) {
                minutesSinceNewest = 10;
            }
            var fetchMinutes = minutesSinceNewest + 5;
            var maxCount = Math.min(Math.ceil(fetchMinutes / 5) + 1, MAX_READINGS);
            console.log('Incremental fetch: ' + fetchMinutes + ' minutes, max ' + maxCount + ' readings');
            dex.getGlucoseReadings(fetchMinutes, maxCount);
        } else {
            /* Full fetch */
            console.log('Full fetch: 180 minutes, 36 readings');
            dex.getGlucoseReadings(180, 36);
        }
    } catch (error) {
        console.error('Error fetching glucose: ' + error.message);
    }
}

// Listen for when the watchface is opened
Pebble.addEventListener('ready', function() {
    console.log('PebbleKit JS ready!');
    appSettings = getSettings();
    fetchGlucoseData();
});

// Listen for messages from the watch
Pebble.addEventListener('appmessage', function(e) {
    console.log('AppMessage received from watch');
    appSettings = getSettings();
    fetchGlucoseData();
});

// Listen for when settings are closed
Pebble.addEventListener('webviewclosed', function() {
    console.log('Settings closed');
    appSettings = getSettings();
    fetchGlucoseData();
});
