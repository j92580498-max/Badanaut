"""ARM ELF metadata used to choose bundled or imported bada runtimes."""
from pathlib import Path
import struct

CPP = "libstdc++.so.6"
GCC = "libgcc_s.so.1"
SUPCXX = "libsupc++.so"


def _attributes(data):
    if not data or data[0] != ord("A"):
        raise ValueError("Invalid ARM attributes")

    def word(offset, end):
        if offset + 4 > end:
            raise ValueError("Truncated ARM attributes")
        return struct.unpack_from("<I", data, offset)[0]

    def uleb(offset, end):
        value = 0
        for shift in range(0, 35, 7):
            if offset >= end:
                break
            byte = data[offset]
            offset += 1
            value |= (byte & 127) << shift
            if byte < 128 and value <= 0xffffffff:
                return value, offset
        raise ValueError("Invalid ARM attribute value")

    def string_end(offset, end):
        terminator = data.find(b"\0", offset, end)
        if terminator < 0:
            raise ValueError("Truncated ARM attribute string")
        return terminator + 1

    offset, abi = 1, "unknown"
    while offset < len(data):
        end = offset + word(offset, len(data))
        if end <= offset + 4 or end > len(data):
            raise ValueError("Invalid ARM attribute section length")
        vendor_end = string_end(offset + 4, end)
        if data[offset + 4:vendor_end - 1] != b"aeabi":
            offset = end
            continue
        offset = vendor_end
        while offset < end:
            start = offset
            scope, offset = uleb(offset, end)
            scope_end = start + word(offset, end)
            offset += 4
            if scope_end < offset or scope_end > end:
                raise ValueError("Invalid ARM attribute scope length")
            if scope == 1:
                current = "soft"  # Missing Tag_ABI_VFP_args means base AAPCS.
                while offset < scope_end:
                    tag, offset = uleb(offset, scope_end)
                    if tag in (4, 5) or (tag >= 32 and tag % 2):
                        offset = string_end(offset, scope_end)
                    else:
                        value, offset = uleb(offset, scope_end)
                        if tag == 28:
                            current = {0: "soft", 1: "hard", 2: "unknown", 3: "compatible"}.get(value, "unknown")
                        if tag == 32:
                            offset = string_end(offset, scope_end)
                if abi != "unknown" and current != abi:
                    raise ValueError("Conflicting ARM float attributes")
                abi = current
            offset = scope_end
        offset = end
    return abi


def float_abi(path):
    """Read the calling convention, independently of the SDK/API version.

    Samsung ELF flags frequently omit the float flags. The aeabi file
    attributes, particularly Tag_ABI_VFP_args, are authoritative in that case.
    A stripped, older installation with no attributes remains 'unknown'.
    """
    with Path(path).open("rb") as source:
        header = source.read(52)
        if len(header) != 52 or header[:7] != b"\x7fELF\x01\x01\x01" or header[16:20] != b"\x03\x00\x28\x00":
            raise ValueError("Not a little-endian ARM32 ELF library")
        source.seek(0, 2)
        size = source.tell()
        flags = struct.unpack_from("<I", header, 36)[0]
        flagged = "hard" if flags & 0x400 else "soft" if flags & 0x200 else "unknown"
        offset = struct.unpack_from("<I", header, 32)[0]
        entry_size, count = struct.unpack_from("<HH", header, 46)
        if not count:
            return flagged
        if entry_size < 40 or offset + entry_size * count > size:
            raise ValueError("Invalid ARM ELF section table")
        abi = "unknown"
        for index in range(count):
            source.seek(offset + index * entry_size)
            section = struct.unpack("<10I", source.read(40))
            if section[1] != 0x70000003:  # SHT_ARM_ATTRIBUTES
                continue
            if section[5] > 64 * 1024 or section[4] + section[5] > size:
                raise ValueError("Invalid ARM attributes section")
            source.seek(section[4])
            abi = _attributes(source.read(section[5]))
        if flagged != "unknown" and abi not in ("unknown", "compatible", flagged):
            raise ValueError("Conflicting ARM float ABI metadata")
        return flagged if abi == "unknown" else abi


def _symbol_names(entries, strings, entry_size, defined):
    if entry_size < 16 or len(entries) % entry_size:
        raise ValueError("Invalid ARM ELF dynamic symbols")
    result = set()
    for start in range(0, len(entries), entry_size):
        name, _, _, _, _, index = struct.unpack_from("<IIIBBH", entries, start)
        if bool(index) != defined or not name:
            continue
        end = strings.find(b"\0", name)
        if end < 0:
            raise ValueError("Invalid ARM ELF symbol name")
        result.add(strings[name:end].decode("ascii", "replace"))
    return result


def _dynamic_symbols(source, header, size, defined):
    """Sectionless guests retain their import table through PT_DYNAMIC."""
    offset = struct.unpack_from("<I", header, 28)[0]
    stride, count = struct.unpack_from("<HH", header, 42)
    if not count:
        return set()
    if stride < 32 or offset + stride * count > size:
        raise ValueError("Invalid ARM ELF program table")
    segments, dynamic = [], None
    for index in range(count):
        source.seek(offset + index * stride)
        segment = struct.unpack("<8I", source.read(32))
        if segment[1] + segment[4] > size:
            raise ValueError("Invalid ARM ELF segment")
        if segment[0] == 1:  # PT_LOAD
            segments.append(segment)
        elif segment[0] == 2:  # PT_DYNAMIC
            dynamic = segment
    if dynamic is None:
        return set()

    def read_at(offset, length):
        if length > 32 * 1024 * 1024 or offset + length > size:
            raise ValueError("Invalid ARM ELF dynamic table")
        source.seek(offset)
        return source.read(length)

    def read_virtual(address, length):
        for segment in segments:
            if address >= segment[2] and address + length <= segment[2] + segment[4]:
                return read_at(segment[1] + address - segment[2], length)
        raise ValueError("Invalid ARM ELF dynamic address")

    tags = {}
    entries = read_at(dynamic[1], dynamic[4])
    if len(entries) % 8:
        raise ValueError("Invalid ARM ELF dynamic table")
    for start in range(0, len(entries), 8):
        tag, value = struct.unpack_from("<II", entries, start)
        if not tag:  # DT_NULL
            break
        tags[tag] = value
    if not all(tags.get(tag) for tag in (5, 6, 10)):  # STRTAB, SYMTAB, STRSZ
        raise ValueError("Invalid ARM ELF dynamic symbols")
    entry_size = tags.get(11, 16)  # DT_SYMENT
    if entry_size != 16:
        raise ValueError("Invalid ARM ELF dynamic symbol size")
    if tags.get(4):  # DT_HASH's nchain counts symbols, even if STRTAB is earlier.
        symbol_count = struct.unpack("<II", read_virtual(tags[4], 8))[1]
    else:
        symbol_count = max(0, tags[5] - tags[6]) // entry_size
    if not symbol_count:
        raise ValueError("Invalid ARM ELF dynamic symbol count")
    return _symbol_names(read_virtual(tags[6], symbol_count * entry_size),
                         read_virtual(tags[5], tags[10]), entry_size, defined)


def symbols(path, defined=True):
    """Inspect dynamic definitions/imports without guessing from the filename."""
    with Path(path).open("rb") as source:
        header = source.read(52)
        if len(header) != 52 or header[:7] != b"\x7fELF\x01\x01\x01":
            raise ValueError("Invalid ARM ELF header")
        source.seek(0, 2)
        size = source.tell()
        offset = struct.unpack_from("<I", header, 32)[0]
        stride, count = struct.unpack_from("<HH", header, 46)
        if not count:
            return _dynamic_symbols(source, header, size, defined)
        if stride < 40 or offset + stride * count > size:
            raise ValueError("Invalid ARM ELF section table")
        sections = []
        for index in range(count):
            source.seek(offset + index * stride)
            sections.append(struct.unpack("<10I", source.read(40)))

        def contents(section):
            if section[5] > 32 * 1024 * 1024 or section[4] + section[5] > size:
                raise ValueError("Invalid ARM ELF symbol section")
            source.seek(section[4])
            return source.read(section[5])

        result = set()
        for section in sections:
            if section[1] != 11:  # SHT_DYNSYM
                continue
            entry_size = section[9] or 16
            if section[6] >= count or entry_size < 16 or section[5] % entry_size:
                raise ValueError("Invalid ARM ELF dynamic symbols")
            strings = contents(sections[section[6]])
            entries = contents(section)
            result.update(_symbol_names(entries, strings, entry_size, defined))
        return result


def runtime_filename(path):
    exports = symbols(path)
    if {"_Unwind_RaiseException", "__gnu_unwind_frame"} <= exports:
        return GCC
    if "__cxa_throw" in exports and not any(name.startswith(("_ZNSs", "_ZNSt6locale", "_ZNSt7__cxx11")) for name in exports):
        return SUPCXX
    return CPP


def game_float_abi(path):
    """Use the same fallback as the native loader for older stripped games."""
    abi = float_abi(path)
    if abi in ("hard", "soft"):
        return abi
    return "soft" if {"__aeabi_fmul", "__aeabi_fadd"} & symbols(path, defined=False) else "hard"


def bundle_roots(engine_directory):
    directory = Path(engine_directory).resolve()
    roots = [directory / "bada_libs"]
    for parent in (directory, *directory.parents):
        if parent.name == ".build":
            roots.append(parent.parent / "bada_libs")
            break
    return roots
