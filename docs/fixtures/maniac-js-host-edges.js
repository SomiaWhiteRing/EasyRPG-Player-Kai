(() => {
    const rows = [];
    const events = [];
    const types = [
        ["number", getv, setv, 77],
        ["switch", gets, sets, true],
        ["string", gett, sett, "before"]
    ];
    const inputs = [
        ["undefined", () => undefined], ["null", () => null],
        ["false", () => false], ["true", () => true],
        ["fraction", () => 0.5], ["negative_fraction", () => -3.7],
        ["nan", () => NaN], ["infinity", () => Infinity],
        ["wrap32", () => 4294967297], ["numeric_string", () => "12"],
        ["bad_string", () => "bad"], ["bigint", () => 42n],
        ["symbol", () => Symbol("probe")],
        ["object", () => ({
            valueOf() { events.push("valueOf"); return 23; },
            toString() { events.push("toString"); return "object"; }
        })],
        ["throwing_object", () => ({
            valueOf() { events.push("valueOf!"); throw new Error("number-fail"); },
            toString() { events.push("toString!"); throw new Error("string-fail"); }
        })]
    ];
    function normalized(value) {
        if (value === undefined) return {type: "undefined"};
        if (typeof value === "number" && !Number.isFinite(value)) return {type: String(value)};
        if (typeof value === "bigint") return {type: "bigint", value: String(value)};
        if (Array.isArray(value)) return value.map(normalized);
        return value;
    }
    function run(name, operation, observe) {
        const selected = globalThis.__edge_case;
        if (selected ? name !== selected : name.endsWith("/error/id_proxy_get")) return;
        setv(2, rows.length + 1);
        setv(3, 1);
        sett(2, name);
        events.length = 0;
        let outcome;
        try { outcome = {returned: normalized(operation())}; }
        catch (error) { outcome = {thrown: String(error)}; }
        setv(3, 2);
        const operationEvents = events.slice();
        let after;
        try { after = normalized(observe()); }
        catch (error) { after = {thrown: String(error)}; }
        setv(3, 3);
        rows.push({name, outcome, events: operationEvents, after});
    }
    for (const [type, get, set, initial] of types) {
        for (const [name, make] of inputs) {
            set(10, initial);
            run(type + "/scalar/" + name, () => set(10, make()), () => get(10));
        }
    }
    for (const [type, get, set, initial] of types) {
        const values = type === "string" ? ["A", "B", "C"] :
                       type === "switch" ? [1, 0, 1] : [11, 22, 33];
        const reset = () => { for (let i = 0; i < 3; ++i) set(10 + i, initial); };
        const snapshot = () => [get(10), get(11), get(12)];
        reset();
        run(type + "/array/values", () => set(10, values), snapshot);
        run(type + "/array/range_descriptor", () => get([10, 12, 11]), snapshot);
        run(type + "/array/range_pair", () => get([10, 11]), snapshot);
        run(type + "/array/range", () => get(10, 3), snapshot);
        reset();
        const proxyValues = new Proxy(values, {
            get(target, key, receiver) {
                events.push("value:" + String(key));
                return Reflect.get(target, key, receiver);
            }
        });
        run(type + "/proxy/values", () => set(10, proxyValues), snapshot);
        const proxyIds = new Proxy([10, 12, 11], {
            get(target, key, receiver) {
                events.push("id:" + String(key));
                return Reflect.get(target, key, receiver);
            }
        });
        run(type + "/proxy/range_descriptor", () => get(proxyIds), snapshot);
        reset();
        const bad = {
            valueOf() { events.push("bad:valueOf"); throw new Error("element-number"); },
            toString() { events.push("bad:toString"); throw new Error("element-string"); }
        };
        const partial = [values[0], bad, values[2]];
        run(type + "/error/value_conversion", () => set(10, partial), snapshot);
        reset();
        const throwingValues = new Proxy(values, {
            get(target, key, receiver) {
                events.push("value:" + String(key));
                if (key === "1") throw new Error("value-get");
                return Reflect.get(target, key, receiver);
            }
        });
        run(type + "/error/value_proxy_get", () => set(10, throwingValues), snapshot);
        const throwingIds = new Proxy([10, 11, 12], {
            get(target, key, receiver) {
                events.push("id:" + String(key));
                if (key === "1") throw new Error("id-get");
                return Reflect.get(target, key, receiver);
            }
        });
        run(type + "/error/id_proxy_get", () => get(throwingIds), snapshot);
        reset();
        run(type + "/order/id_before_value", () => set({
            valueOf() { events.push("id:valueOf"); return 10; }
        }, {
            valueOf() { events.push("value:valueOf"); return 25; },
            toString() { events.push("value:toString"); return "ordered"; }
        }), snapshot);
        reset();
        run(type + "/error/id_conversion", () => set({
            valueOf() { events.push("id:valueOf!"); throw new Error("id-convert"); }
        }, {
            valueOf() { events.push("value:valueOf"); return 25; },
            toString() { events.push("value:toString"); return "ordered"; }
        }), snapshot);
        reset();
        const revoked = Proxy.revocable(values, {});
        revoked.revoke();
        run(type + "/error/revoked_values", () => set(10, revoked.proxy), snapshot);
    }
    return JSON.stringify(rows);
})()
