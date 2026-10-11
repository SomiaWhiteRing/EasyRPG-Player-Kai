"""Audit the frozen original 250921 save without rewriting its unknown chunks."""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs" / "evidence"
SAVE = EVIDENCE / "maniac-250921-original-save01.lsd"
MANIFEST = EVIDENCE / "maniac-250921-save-manifest.json"
TRACE = EVIDENCE / "maniac-250921-save-trace.json"
OUTPUT = EVIDENCE / "maniac-250921-save-audit.json"
VARIANTS = {
    "full": (SAVE, MANIFEST, TRACE),
    "minimal": (EVIDENCE / "maniac-250921-original-save-minimal.lsd",
                EVIDENCE / "maniac-250921-save-minimal-manifest.json",
                EVIDENCE / "maniac-250921-save-minimal-trace.json"),
    "map-self": (EVIDENCE / "maniac-250921-original-save-map-self.lsd",
                 EVIDENCE / "maniac-250921-save-map-self-manifest.json",
                 EVIDENCE / "maniac-250921-save-map-self-trace.json"),
    "map-self-event2": (EVIDENCE / "maniac-250921-original-save-map-self-event2.lsd",
                        EVIDENCE / "maniac-250921-save-map-self-event2-manifest.json",
                        EVIDENCE / "maniac-250921-save-map-self-event2-trace.json"),
    "map-self-string": (EVIDENCE / "maniac-250921-original-save-map-self-string.lsd",
                        EVIDENCE / "maniac-250921-save-map-self-string-manifest.json",
                        EVIDENCE / "maniac-250921-save-map-self-string-trace.json"),
    "map-self-strings": (EVIDENCE / "maniac-250921-original-save-map-self-strings.lsd",
                         EVIDENCE / "maniac-250921-save-map-self-strings-manifest.json",
                         EVIDENCE / "maniac-250921-save-map-self-strings-trace.json"),
    "map-self-resume": (EVIDENCE / "maniac-250921-original-save-map-self-resume.lsd",
                        EVIDENCE / "maniac-250921-save-map-self-resume-manifest.json",
                        EVIDENCE / "maniac-250921-save-map-self-resume-trace.json"),
    "map-self-two-events": (EVIDENCE / "maniac-250921-original-save-map-self-two-events.lsd",
                            EVIDENCE / "maniac-250921-save-map-self-two-events-manifest.json",
                            EVIDENCE / "maniac-250921-save-map-self-two-events-trace.json"),
    "map-self-two-maps": (EVIDENCE / "maniac-250921-original-save-map-self-two-maps.lsd",
                          EVIDENCE / "maniac-250921-save-map-self-two-maps-manifest.json",
                          EVIDENCE / "maniac-250921-save-map-self-two-maps-trace.json"),
    "map-switch": (EVIDENCE / "maniac-250921-original-save-map-switch.lsd",
                   EVIDENCE / "maniac-250921-save-map-switch-manifest.json",
                   EVIDENCE / "maniac-250921-save-map-switch-trace.json"),
}


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def ber(blob: bytes, position: int) -> tuple[int, int]:
    value = 0
    for _ in range(6):
        if position >= len(blob):
            raise ValueError("truncated BER integer")
        octet = blob[position]
        position += 1
        value = (value << 7) | (octet & 0x7F)
        if not octet & 0x80:
            return value, position
    raise ValueError("oversized BER integer")


def chunks(blob: bytes, position: int, end: int) -> list[dict[str, int | str]]:
    result = []
    while position < end:
        if position == end - 1 and blob[position] == 0:
            break
        start = position
        chunk_id, position = ber(blob, position)
        size, position = ber(blob, position)
        if position + size > end:
            raise ValueError(f"chunk {chunk_id} exceeds container")
        payload = blob[position:position + size]
        result.append({"id": chunk_id, "offset": start, "length": size,
                       "payload_offset": position, "sha256": sha256(payload)})
        position += size
    return result


def decode_banks(payload: bytes, label: str) -> list[dict]:
    """Read the observed indexed bank entries, each ended by a zero field ID."""
    position = 0
    count, position = ber(payload, position)
    if count > 10000:
        raise ValueError(f"{label}: excessive bank count")
    banks = []
    for _ in range(count):
        bank_id, position = ber(payload, position)
        if bank_id == 0:
            raise ValueError(f"{label}: zero bank ID")
        fields = {}
        while True:
            field_id, position = ber(payload, position)
            if field_id == 0:
                break
            if field_id in fields:
                raise ValueError(f"{label}: duplicate field {field_id}")
            size, position = ber(payload, position)
            if position + size > len(payload):
                raise ValueError(f"{label}: truncated field {field_id}")
            fields[field_id] = payload[position:position + size]
            position += size
        banks.append({"id": bank_id, "fields": fields})
    if position != len(payload):
        raise ValueError(f"{label}: bytes remain after {count} banks")
    return banks


def decode_values(fields: dict[int, bytes], label: str) -> dict:
    result = {}
    if 1 in fields:
        raw = fields[1]
        switches = []
        position, value = 0, 1
        while position < len(raw):
            count, position = ber(raw, position)
            if not switches and count >= (1 << 35) - 1000000:
                count, value = (1 << 35) - count, 0
            if count == 0 or count > 1000000 - len(switches):
                raise ValueError(f"{label}: invalid switch run length")
            switches.extend([value] * count)
            value = 1 - value
        result["switch_bytes"] = switches
    if 2 in fields:
        raw = fields[2]
        if len(raw) % 4:
            raise ValueError(f"{label}: numeric vector is not int32 aligned")
        result["numbers_positive_slots"] = list(struct.unpack("<" + "i" * (len(raw) // 4), raw))
    if 3 in fields:
        raw = fields[3]
        strings = []
        position = 0
        while position < len(raw):
            units, position = ber(raw, position)
            if units == 0:
                continue  # Native loader does not append a slot for zero records.
            if units >= (1 << 35) - 1000000:
                count = (1 << 35) - units
                if count > 1000000 - len(strings):
                    raise ValueError(f"{label}: excessive empty string run length")
                strings.extend([None] * count)
            else:
                if units > (len(raw) - position) // 2:
                    raise ValueError(f"{label}: UTF-16 string exceeds field 3")
                strings.append(raw[position:position + 2 * units].decode("utf-16-le", errors="surrogatepass"))
                position += 2 * units
        result["field_3_observed_utf16_slots"] = strings
    for field_id, raw in fields.items():
        if field_id not in (1, 2, 3):
            result[f"unknown_field_{field_id}_hex"] = raw.hex()
    return result


def decode_common_self(blob: bytes, child: dict[str, int | str]) -> list[dict]:
    payload = blob[int(child["payload_offset"]):int(child["payload_offset"]) + int(child["length"])]
    return [{"common_event_id": entry["id"], **decode_values(entry["fields"], "common self")}
            for entry in decode_banks(payload, "common self")]


def decode_map_self_numbers(blob: bytes, child: dict[str, int | str]) -> list[dict]:
    """Decode the observed root 0x74 child 2 nested map/event bank fields."""
    payload = blob[int(child["payload_offset"]):int(child["payload_offset"]) + int(child["length"])]
    maps = []
    for map_entry in decode_banks(payload, "map self"):
        fields = map_entry["fields"]
        if 2 not in fields:
            raise ValueError("map self event list field 2 absent")
        events = [{"event_id": entry["id"], **decode_values(entry["fields"], "event self")}
                  for entry in decode_banks(fields[2], "event self")]
        maps.append({"map_id": map_entry["id"], "events": events,
                     "other_map_fields_hex": {str(key): value.hex() for key, value in fields.items() if key != 2}})
    return maps


def inspect(name: str, save_path: Path, manifest_path: Path, trace_path: Path) -> dict:
    blob = save_path.read_bytes()
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace = json.loads(trace_path.read_text(encoding="utf-8"))
    if not manifest["save_probe"] or trace["fixture_manifest_sha256"] != sha256(manifest_bytes):
        raise ValueError("frozen save manifest/trace mismatch")
    if trace["reference_sha256"] != manifest["files"]["RPG_RT.exe"]:
        raise ValueError("reference executable hash mismatch")
    if name == "map-switch" and (
            manifest.get("expected_save_slot") != 1 or
            trace.get("save_file_sha256") != sha256(blob)):
        raise ValueError("map-switch save is not bound to its original run")
    if not any(sample["vector_length"] == manifest["expected_global_numeric_vector_length_after_event"]
               and sample["slots"] == manifest["expected_global_slots_after_event"]
               for sample in trace["samples"]):
        raise ValueError("original runtime did not reach the expected post-save state")
    header_length, position = ber(blob, 0)
    header = blob[position:position + header_length]
    if header != b"LcfSaveData":
        raise ValueError("not an LSD save")
    root_chunks = chunks(blob, position + header_length, len(blob))
    root_by_id = {entry["id"]: entry for entry in root_chunks}
    if len(root_by_id) != len(root_chunks) or 101 not in root_by_id:
        raise ValueError("expected original save chunks absent")
    party = root_by_id[104]
    party_fields = {entry["id"]: entry for entry in chunks(
        blob, int(party["payload_offset"]), int(party["payload_offset"]) + int(party["length"]))}
    map_field = party_fields[11]
    map_payload = blob[int(map_field["payload_offset"]):int(map_field["payload_offset"]) + int(map_field["length"])]
    saved_map_id, map_bytes_used = ber(map_payload, 0)
    if map_bytes_used != len(map_payload):
        raise ValueError("saved party map ID has trailing bytes")
    if name == "map-switch" and saved_map_id != 2:
        raise ValueError("map-switch save did not reach map 2")
    system = root_by_id[101]
    system_chunks = chunks(blob, int(system["payload_offset"]),
                           int(system["payload_offset"]) + int(system["length"]))
    system_by_id = {entry["id"]: entry for entry in system_chunks}
    if 134 not in system_by_id or system_by_id[134]["length"] != 1:
        raise ValueError("expected original SaveSystem chunk 0x86 absent")
    original_extension = root_by_id.get(116)
    extension_children = (chunks(blob, int(original_extension["payload_offset"]),
                                 int(original_extension["payload_offset"]) + int(original_extension["length"]))
                          if original_extension else [])
    expected_children = {"full": [(1, 17)], "minimal": [], "map-self": [],
                         "map-self-event2": [], "map-self-string": [], "map-self-strings": [],
                         "map-self-resume": [], "map-self-two-events": [],
                         "map-self-two-maps": [], "map-switch": []}[name]
    if name != "minimal":
        expected_children += [(2, {"full": 31, "map-self-string": 39,
                                  "map-self-strings": 47, "map-self-two-events": 22,
                                  "map-self-two-maps": 27, "map-switch": 27}.get(name, 14))]
    if [(entry["id"], entry["length"]) for entry in extension_children] != expected_children:
        raise ValueError(f"unexpected original root extension shape: {name}")
    if bool(original_extension) != bool(expected_children):
        raise ValueError(f"unexpected root extension presence: {name}")
    map_numbers = (decode_map_self_numbers(blob, next(entry for entry in extension_children if entry["id"] == 2))
                   if original_extension else [])
    common_banks = (decode_common_self(blob, next(entry for entry in extension_children if entry["id"] == 1))
                    if any(entry["id"] == 1 for entry in extension_children) else [])
    if name == "full" and common_banks != [
            {"common_event_id": 1, "switch_bytes": [1], "numbers_positive_slots": [70]},
            {"common_event_id": 2, "switch_bytes": [1]}]:
        raise ValueError(f"full common self banks mismatch: {common_banks}")
    if name != "full" and common_banks:
        raise ValueError(f"reduced save has common self banks: {name}")
    if name == "full" and map_numbers != [{"map_id": 1, "events": [
            {"event_id": 1, "switch_bytes": [1, 1, 0, 0, 1],
             "numbers_positive_slots": [0, 0, 88, 5]}], "other_map_fields_hex": {}}]:
        raise ValueError(f"full map self banks mismatch: {map_numbers}")
    if name == "minimal" and map_numbers:
        raise ValueError("minimal save unexpectedly has self numbers")
    if name in {"map-self", "map-self-event2", "map-self-resume"}:
        expected_event_id = 2 if name == "map-self-event2" else 1
        if map_numbers != [{"map_id": 1, "events": [{"event_id": expected_event_id,
                                                      "numbers_positive_slots": [42]}],
                            "other_map_fields_hex": {}}]:
            raise ValueError(f"reduced self numeric vector mismatch: {name}: {map_numbers}")
    if name == "map-self-string" and map_numbers != [{"map_id": 1, "events": [
            {"event_id": 1, "numbers_positive_slots": [42],
             "field_3_observed_utf16_slots": ["Bank sample"]}], "other_map_fields_hex": {}}]:
        raise ValueError(f"map self string sample mismatch: {map_numbers}")
    if name == "map-self-strings" and map_numbers != [{"map_id": 1, "events": [
            {"event_id": 1, "numbers_positive_slots": [42],
             "field_3_observed_utf16_slots": ["Bank sample", None, "Z"]}], "other_map_fields_hex": {}}]:
        raise ValueError(f"map self strings sample mismatch: {map_numbers}")
    if name == "map-self-two-events" and map_numbers != [{"map_id": 1, "events": [
            {"event_id": 1, "numbers_positive_slots": [42]},
            {"event_id": 2, "numbers_positive_slots": [43]}], "other_map_fields_hex": {}}]:
        raise ValueError(f"map self two-event sample mismatch: {map_numbers}")
    if name in {"map-self-two-maps", "map-switch"} and map_numbers != [
            {"map_id": 1, "events": [{"event_id": 1, "numbers_positive_slots": [42]}],
             "other_map_fields_hex": {}},
            {"map_id": 2, "events": [{"event_id": 2, "numbers_positive_slots": [43]}],
             "other_map_fields_hex": {}}]:
        raise ValueError(f"map self two-map sample mismatch: {map_numbers}")
    return {
        "variant": name,
        "reference_sha256": trace["reference_sha256"],
        "manifest_sha256": sha256(manifest_bytes),
        "save_sha256": sha256(blob),
        "save_length": len(blob),
        "saved_party_map_id": saved_map_id,
        "save_system_unknown_chunk_0x86": system_by_id[134],
        "root_extension_116": original_extension,
        "root_extension_116_children": extension_children,
        "common_self_banks": common_banks,
        "map_self_numeric_vectors": map_numbers,
    }


def main() -> None:
    variants = {name: inspect(name, *files) for name, files in VARIANTS.items()}
    first = (EVIDENCE / "maniac-250921-original-save-map-self.lsd").read_bytes()
    second = (EVIDENCE / "maniac-250921-original-save-map-self-event2.lsd").read_bytes()
    first_chunk = variants["map-self"]["root_extension_116_children"][0]
    second_chunk = variants["map-self-event2"]["root_extension_116_children"][0]
    first_payload = first[first_chunk["payload_offset"]:first_chunk["payload_offset"] + first_chunk["length"]]
    second_payload = second[second_chunk["payload_offset"]:second_chunk["payload_offset"] + second_chunk["length"]]
    changed = [index for index, (left, right) in enumerate(zip(first_payload, second_payload)) if left != right]
    if len(first_payload) != len(second_payload) or changed != [5] or first_payload[5] != 1 or second_payload[5] != 2:
        raise ValueError("event ID change is not isolated to expected root extension byte")
    negative_manifest_bytes = (EVIDENCE / "maniac-250921-save-negative-result-manifest.json").read_bytes()
    negative_manifest = json.loads(negative_manifest_bytes)
    negative_trace = json.loads((EVIDENCE / "maniac-250921-save-negative-result-trace.json").read_text(encoding="utf-8"))
    if negative_trace["fixture_manifest_sha256"] != sha256(negative_manifest_bytes) or not negative_manifest["save_probe"]:
        raise ValueError("negative-result fixture provenance mismatch")
    negative_snapshots = []
    for slot in (1, 2):
        path = EVIDENCE / f"maniac-250921-original-save-negative-result-0{slot}.lsd"
        blob = path.read_bytes()
        header_length, position = ber(blob, 0)
        root = {entry["id"]: entry for entry in chunks(blob, position + header_length, len(blob))}
        extension = root[116]
        children = chunks(blob, int(extension["payload_offset"]),
                          int(extension["payload_offset"]) + int(extension["length"]))
        map_child = next(entry for entry in children if entry["id"] == 2)
        map_numbers = decode_map_self_numbers(blob, map_child)
        negative_snapshots.append({"slot": slot, "sha256": sha256(blob), "length": len(blob),
                                   "map_self_payload_length": map_child["length"],
                                   "map_self_numeric_vectors": map_numbers})
    if [entry["map_self_payload_length"] for entry in negative_snapshots] != [31, 39]:
        raise ValueError("negative self result did not change the second save's map bank shape")
    first_values = negative_snapshots[0]["map_self_numeric_vectors"][0]["events"][0]["numbers_positive_slots"]
    second_values = negative_snapshots[1]["map_self_numeric_vectors"][0]["events"][0]["numbers_positive_slots"]
    if first_values != [0, 0, 88, 5] or second_values != [0, 0, 88, 5, 0, 1]:
        raise ValueError("negative self result numeric slot 6 was not isolated")
    if any(snapshot["map_self_numeric_vectors"][0]["events"][0].get("switch_bytes") != [1, 1, 0, 0, 1]
           for snapshot in negative_snapshots):
        raise ValueError("negative result unexpectedly changed map self switch bytes")
    if not any(sample["slots"] == negative_manifest["expected_global_slots_after_event"]
               and sample["vector_length"] == 39 for sample in negative_trace["samples"]):
        raise ValueError("negative-result original runtime trace incomplete")
    audit = {
        "variants": variants,
        "negative_result": {
            "reference_sha256": negative_trace["reference_sha256"],
            "manifest_sha256": sha256(negative_manifest_bytes),
            "snapshots": negative_snapshots,
            "deduction": "The first save snapshots before self result -6 is written. The second save, taken after reading that result into global ID37, has a longer map self payload. Original process ID37 is 1.",
        },
        "resume_comparison": {
            "variant": "map-self-resume",
            "original_after_save_physical_slots": [7, 7, 0, 42, 42],
            "saved_bank_numbers_positive_slots": [42],
            "player_post_load_global_id_4": 42,
            "scope": "One observed map event numeric self read after save, not full original save interoperability",
        },
        "two_event_map_bank": {
            "variant": "map-self-two-events",
            "map_id": 1,
            "event_ids": [1, 2],
            "numbers_positive_slots": [[42], [43]],
        },
        "two_map_banks": {
            "variant": "map-self-two-maps",
            "map_event_numbers": [[1, 1, 42], [2, 2, 43]],
        },
        "map_switch_lifecycle": {
            "variant": "map-switch",
            "original_saved_bank_map_event_numbers": [[1, 1, 42], [2, 2, 43]],
            "scope": "Teleport from map 1 to map 2 followed by the second map's autostart save",
        },
        "event_id_change": {"root_0x74_child_2_payload_relative_byte": 5, "from": 1, "to": 2},
        "deduction": "In observed samples, root 0x74 child 1 is an indexed common-event bank list, and child 2 is an indexed map/event bank list. Each bank uses field 1 as a byte vector and field 2 as a little-endian int32 vector. Map event field 3 contains consecutive UTF-16LE string slots, each prefixed by a BER code-unit count; an unallocated middle slot has five-byte BER sentinel ff ff ff ff 7f. Changing the map event ID from 1 to 2 changes only byte 5 of child 2's payload. The map-switch run retains map 1's bank when map 2 saves. Other field formats, physical slot mapping, and lifecycle cases remain unconfirmed.",
        "note": "Only observed bank fields are decoded. liblcf preserves raw original 0x74 bytes and Player imports these observed banks, but a parse/write round trip does not prove original interoperability.",
    }
    OUTPUT.write_text(json.dumps(audit, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for name, item in variants.items():
        extension = item["root_extension_116"]
        print(f"{name}: {item['save_length']} bytes; root 0x74="
              f"{extension['length'] if extension else 'absent'}; "
              f"children={[(child['id'], child['length']) for child in item['root_extension_116_children']]}")


if __name__ == "__main__":
    main()
