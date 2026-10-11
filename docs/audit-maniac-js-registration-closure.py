"""Bind fixed original JS registration startup calls to PE sites and names."""

from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
import pefile


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
TRACE = ROOT / "docs/evidence/maniac-250921-js-registration-closure-trace.json"
PRIOR = ROOT / "docs/evidence/maniac-250921-js-registration-trace.json"
REACHABILITY = ROOT / "docs/evidence/maniac-250921-js-registration-reachability.json"
DYNAMIC = ROOT / "docs/evidence/maniac-250921-js-dynamic-registration-trace.json"
BINDINGS = ROOT / "docs/evidence/maniac-250921-js-host-registration-audit.json"
MANIFEST = ROOT / "docs/evidence/maniac-250921-js-host-symbols-manifest.json"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-registration-closure-audit.json"
EXPECTED_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
HOST_RANGES = ((0x17E030, 0x17E575), (0x1803E0, 0x180CFB), (0x181650, 0x18199C))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if sha256(RUNTIME) != EXPECTED_SHA256:
        raise ValueError("fixed original runtime changed")
    trace = json.loads(TRACE.read_text())
    prior = json.loads(PRIOR.read_text())
    reachability = json.loads(REACHABILITY.read_text())
    dynamic = json.loads(DYNAMIC.read_text())
    bindings = json.loads(BINDINGS.read_text())
    manifest_hash = sha256(MANIFEST)
    if (trace["runtime_sha256"] != EXPECTED_SHA256
            or trace["fixture_manifest_sha256"] != manifest_hash
            or prior["runtime_sha256"] != EXPECTED_SHA256
            or prior["fixture_manifest_sha256"] != manifest_hash
            or reachability["runtime_sha256"] != EXPECTED_SHA256
            or dynamic["runtime_sha256"] != EXPECTED_SHA256
            or dynamic["fixture_manifest_sha256"] != manifest_hash
            or bindings["runtime_sha256"] != EXPECTED_SHA256):
        raise ValueError("startup trace provenance changed")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    enriched = []
    seen = set()
    for event in trace["events"]:
        site = int(event["call_site_rva"], 16)
        target = int(event["target_rva"], 16)
        if (site, target) in seen or event["count"] < 1:
            raise ValueError(f"duplicate or empty registration observation: {event}")
        seen.add((site, target))
        insn = list(decoder.disasm(image.get_data(site, 5), base + site, count=1))
        if (len(insn) != 1 or insn[0].mnemonic != "call" or insn[0].size != 5
                or insn[0].operands[0].type != X86_OP_IMM
                or insn[0].operands[0].imm - base != target):
            raise ValueError(f"observed call not bound to original instruction: {event}")
        name = None
        if event.get("name_rva") is not None:
            name_rva = int(event["name_rva"], 16)
            raw = image.get_data(name_rva, 96)
            if not raw or b"\0" not in raw:
                raise ValueError(f"static registration name inaccessible: {event}")
            name = raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        callback = event.get("callback_rva")
        if callback is not None and not image.get_data(int(callback, 16), 1):
            raise ValueError(f"callback outside original image: {event}")
        for field in ("descriptor_value_10_rva", "descriptor_value_18_rva"):
            address = event.get(field)
            if address is not None and not image.get_data(int(address, 16), 1):
                raise ValueError(f"descriptor field outside original image: {event}")
        host_path = any(lo <= site < hi for lo, hi in HOST_RANGES)
        enriched.append({"call_site_rva": event["call_site_rva"], "target_rva": event["target_rva"],
                         "call_count": event["count"], "in_direct_host_initializer": host_path,
                         "descriptor_rva": event.get("descriptor_rva"),
                         "descriptor_type": event.get("descriptor_type"),
                         "descriptor_value_10_rva": event.get("descriptor_value_10_rva"),
                         "descriptor_value_18_rva": event.get("descriptor_value_18_rva"),
                         "name_rva": event.get("name_rva"), "name": name,
                         "callback_rva": callback, "object_tag": event.get("object_tag")})
    prior_sites = {(int(e["call_site_rva"], 16), int(e["target_rva"], 16)) for e in prior["events"]}
    if prior_sites != {pair for pair in seen if any(lo <= pair[0] < hi for lo, hi in HOST_RANGES)}:
        raise ValueError("global startup hook changed the earlier direct-host observation")
    counts = Counter(e["target_rva"] for e in enriched)
    dynamic_names = [e["call_site_rva"] for e in enriched if e["name_rva"] is None]
    attributed = {e["fresh_startup_helper_call_rva"]: e["property"] for e in reachability["alternate_property_paths"]}
    if not set(attributed).issubset(dynamic_names):
        raise ValueError("known dynamic host property assignments changed")
    generic_dynamic = sorted(set(dynamic_names) - set(attributed))
    actual_dynamic = [e for e in dynamic["events"] if e["call_site_rva"] in generic_dynamic]
    if {e["call_site_rva"] for e in actual_dynamic} != set(generic_dynamic):
        raise ValueError("generic dynamic-name registration trace incomplete")
    expected_picture_accessors = {("get id", "0x17e5d0"), ("get file", "0x17e6f0"),
                                  ("set file", "0x17e8f0")}
    if not expected_picture_accessors.issubset({(e["name"], e["callback_rva"]) for e in actual_dynamic}):
        raise ValueError("picture accessors differ from live descriptors")
    picture_descriptors = {e["name"]: e for e in enriched if e["call_site_rva"] in
                           {"0x17e358", "0x17e3a7", "0x17e3f6", "0x17e445", "0x17e494", "0x17e4e3"}}
    if (picture_descriptors["id"]["descriptor_value_10_rva"] != "0x17e5d0"
            or picture_descriptors["file"]["descriptor_value_10_rva"] != "0x17e6f0"
            or picture_descriptors["file"]["descriptor_value_18_rva"] != "0x17e8f0"):
        raise ValueError("live picture descriptor getter/setter fields changed")
    expected_picture = {"id": "0x17e5d0", "file": "0x17e6f0", "scale": "0x17ea80",
                        "show": "0x17ebd0", "move": "0x17ed10", "erase": "0x17edb0"}
    registered_picture = {e["name"]: e["callback_rva"] for e in bindings["bindings"]
                          if e["object"] == "Rm2k3_Picture"}
    if registered_picture != expected_picture:
        raise ValueError("corrected direct picture callback table changed")
    if {e["name"]: e["callback_rva"] for e in bindings["secondary_callbacks"]
            if e["object"] == "Rm2k3_Picture"} != {"file": "0x17e8f0"}:
        raise ValueError("picture setter callback changed")
    for name, callback in expected_picture.items():
        descriptor = picture_descriptors[name]
        field = "descriptor_value_10_rva" if name in {"id", "file"} else "descriptor_value_18_rva"
        if descriptor[field] != callback:
            raise ValueError(f"live picture descriptor differs from direct table: {name}")
    result = {"runtime_sha256": EXPECTED_SHA256, "fixture_manifest_sha256": manifest_hash,
              "trace_sha256": sha256(TRACE), "prior_direct_host_trace_sha256": sha256(PRIOR),
              "dynamic_registration_trace_sha256": sha256(DYNAMIC),
              "corrected_direct_binding_audit_sha256": sha256(BINDINGS),
              "distinct_call_sites": len(enriched), "direct_host_sites": len(prior_sites),
              "other_startup_sites": len(enriched) - len(prior_sites),
              "by_helper": dict(sorted(counts.items())),
              "dynamic_or_attributed_elsewhere_name_sites": dynamic_names,
              "dynamic_host_properties_from_prior_static_audit": attributed,
              "generic_dynamic_name_sites_attributed_by_live_arguments": generic_dynamic,
              "generic_dynamic_calls": actual_dynamic,
              "calls": enriched,
              "scope": "One fixed original startup reached 155 distinct direct-call sites for three registration helpers. Every observed site and callback is bound to the fixed PE; static names are decoded from PE data. The earlier 23 direct-host observations are exactly the sites inside the host initializer ranges. Other sites include standard-language-library initialization, generic helper internals, and initialization functions invoked before the host table. Six generic dynamic-name call sites are attributed by live native-function creation arguments, including picture id/file getter/setter; two null names remain null. Unexecuted branches and target-object provenance still require static closure before M0-028 can close."}
    if len(enriched) != 155 or len(prior_sites) != 23 or counts != {"0x139db0": 117, "0xfac60": 33, "0x101790": 5}:
        raise ValueError("startup registration call counts changed")
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(enriched)} PE-bound registration sites; {len(prior_sites)} direct host, {len(enriched)-len(prior_sites)} other")


if __name__ == "__main__":
    main()
