"""Guest image abstraction for the lifter: a PE file or a raw flat blob."""
import struct


class Section:
    __slots__ = ("name", "va", "vsize", "rawsize", "flags")

    def __init__(self, name, va, vsize, rawsize, flags):
        self.name, self.va, self.vsize, self.rawsize, self.flags = name, va, vsize, rawsize, flags


class Image:
    """A guest image mapped at `base`; `text` is the [start, end) VA range holding code."""

    def __init__(self, base, data, text_start, text_end, entry, sections, imports, exports=(), pref_base=None):
        self.base = base
        self.pref_base = base if pref_base is None else pref_base  # the PE header's ImageBase
        self.exports = list(exports)  # list of (va, name); forwarders excluded
        self.data = data
        self.text_start = text_start
        self.text_end = text_end
        self.entry = entry
        self.sections = sections
        self.imports = imports  # list of (iat_va, dll, name)
        self.end = base + len(data)
        self.reloc_slots = set()  # addresses of relocated words (PEs with .reloc)

    @classmethod
    def from_pe(cls, path, rebase=None):
        """Load a PE. With `rebase`, apply its base relocations to load it there
        instead of at its preferred base (Granny and Rockall both want 0x10000000)."""
        import pefile

        pe = pefile.PE(path)
        pref = pe.OPTIONAL_HEADER.ImageBase
        base = pref
        if rebase is not None and rebase != pref:
            if not pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size:
                raise SystemExit("%s has no relocations; it cannot be rebased" % path)
            pe.relocate_image(rebase)
            base = rebase
        data = bytes(pe.get_memory_mapped_image())
        size = pe.OPTIONAL_HEADER.SizeOfImage
        if len(data) < size:  # trailing uninitialised data reads as zeros
            data += bytes(size - len(data))
        sections = []
        code = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode("latin1")
            sec = Section(name, base + s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData, s.Characteristics)
            sections.append(sec)
            if s.Characteristics & 0x20:  # IMAGE_SCN_CNT_CODE
                code.append(sec)
        if not code:
            raise SystemExit("no code section in PE")
        # The code range spans every code section (Bink and Miles spread code over
        # several). They must be contiguous: nothing else may lie between them.
        text_start = code[0].va
        text_end = max(c.va + c.vsize for c in code)
        for sec in sections:
            if sec not in code and sec.va < text_end and sec.va + sec.vsize > text_start:
                raise SystemExit("section %s lies between code sections" % sec.name)
        imports = []
        for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = e.dll.decode("latin1").lower()
            for i in e.imports:
                name = i.name.decode("latin1") if i.name else "ord%d" % i.ordinal
                imports.append((i.address, dll, name))  # relocate_image already shifted these
        exports = []
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if e.forwarder or not e.address:
                    continue
                name = e.name.decode("latin1") if e.name else "ord%d" % e.ordinal
                exports.append((base + e.address, name))
        entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        img = cls(base, data, text_start, text_end, entry, sections, imports, exports, pref_base=pref)
        # Addresses of the words the base relocations patch: certainly pointers.
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
        for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in blk.entries:
                if e.type == 3:  # HIGHLOW
                    img.reloc_slots.add(base + e.rva)
        return img

    @classmethod
    def from_raw(cls, path, base):
        data = open(path, "rb").read()
        sec = Section(".text", base, len(data), len(data), 0x60000020)
        return cls(base, data, base, base + len(data), base, [sec], [])

    def is_code_section(self, sec):
        return bool(sec.flags & 0x20)

    def in_text(self, a):
        return self.text_start <= a < self.text_end

    def has(self, a, n=1):
        return self.base <= a and a + n <= self.end

    def u8(self, a):
        return self.data[a - self.base]

    def u32(self, a):
        return struct.unpack_from("<I", self.data, a - self.base)[0]

    def bytes_at(self, a, n):
        o = a - self.base
        return self.data[o : o + n]
