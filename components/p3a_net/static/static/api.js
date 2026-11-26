// SPDX-License-Identifier: Apache-2.0
// Talking to the device: one request function for every page. 1.x had
// seventy hand-written request sites and none looked at an error's code.
//
// Every answer of /api/v2 is {"ok": true, "data": ...} or
// {"ok": false, "error": "words", "code": "CODE"}. A call returns the data
// or throws an ApiError carrying the status, the code and the words.

export class ApiError extends Error {
    constructor(status, code, message) {
        super(message);
        this.name = 'ApiError';
        this.status = status;
        this.code = code;
    }
}

// body: a plain object (sent as JSON), a Blob (sent as it is) or nothing.
export async function api(method, path, body, { timeout = 10000 } = {}) {
    const options = { method, headers: {} };
    if (body instanceof Blob) {
        options.body = body;
    } else if (body !== undefined) {
        options.body = JSON.stringify(body);
        options.headers['Content-Type'] = 'application/json';
    }
    const abort = new AbortController();
    options.signal = abort.signal;
    const timer = setTimeout(() => abort.abort(), timeout);
    let response;
    try {
        response = await fetch(path, options);
    } catch (e) {
        throw e.name === 'AbortError' ? new ApiError(0, 'TIMEOUT', 'The device did not answer.')
            : new ApiError(0, 'NETWORK', 'The device could not be reached.');
    } finally {
        clearTimeout(timer);
    }
    let doc = null;
    try {
        doc = await response.json();
    } catch (e) {
        // Not JSON: an answer from something that is not this firmware.
    }
    if (doc && doc.ok === true) return doc.data;
    if (doc && doc.ok === false) throw new ApiError(response.status, doc.code || 'ERROR', doc.error || `HTTP ${response.status}`);
    throw new ApiError(response.status, 'BAD_ANSWER', `HTTP ${response.status}`);
}

export const get = (path, options) => api('GET', path, undefined, options);
export const post = (path, body, options) => api('POST', path, body, options);
export const put = (path, body, options) => api('PUT', path, body, options);
export const patch = (path, body, options) => api('PATCH', path, body, options);
export const del = (path, options) => api('DELETE', path, undefined, options);

// Waits for a job the device answered 202 with ({"job": id}). Resolves when
// it succeeded; throws an ApiError with the job's error as its code when it
// did not. `onProgress(percent)` is called as it goes.
export async function job(id, onProgress, { every = 700, timeout = 300000 } = {}) {
    const deadline = Date.now() + timeout;
    for (;;) {
        const info = await get(`/api/v2/jobs/${id}`);
        if (onProgress) onProgress(info.progress);
        if (info.state === 'done') {
            if (info.succeeded) return info;
            throw new ApiError(200, info.error || 'FAILED', jobFailure(info.error));
        }
        if (Date.now() > deadline) throw new ApiError(0, 'TIMEOUT', 'That is taking too long.');
        await new Promise((resolve) => setTimeout(resolve, every));
    }
}

function jobFailure(code) {
    switch (code) {
        case 'NOT_PLAYABLE': return 'That is not a picture p3a can play.';
        case 'MISSING': return 'There is nothing at that address.';
        case 'SD_FAILED': return 'The SD card cannot be written.';
        case 'ESP_ERR_NO_MEM': return 'The device is short of memory just now. Try again in a moment.';
        case 'RATE_LIMITED': return 'The server asked to slow down. Try again later.';
        default: return `It did not work (${code || 'unknown error'}).`;
    }
}

// Uploads a file with progress, which fetch cannot report. Resolves with
// the data of the device's answer.
export function upload(path, blob, onProgress) {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', path);
        xhr.upload.onprogress = (e) => {
            if (e.lengthComputable && onProgress) onProgress(Math.round((e.loaded / e.total) * 100));
        };
        xhr.onerror = () => reject(new ApiError(0, 'NETWORK', 'The device could not be reached.'));
        xhr.onload = () => {
            let doc = null;
            try {
                doc = JSON.parse(xhr.responseText);
            } catch (e) {
                // handled below
            }
            if (doc && doc.ok === true) resolve(doc.data);
            else if (doc && doc.ok === false) reject(new ApiError(xhr.status, doc.code || 'ERROR', doc.error || `HTTP ${xhr.status}`));
            else reject(new ApiError(xhr.status, 'BAD_ANSWER', `HTTP ${xhr.status}`));
        };
        xhr.send(blob);
    });
}
