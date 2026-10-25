#include "elf_id.h"
#include <elf.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int read_at(int fd, void *output, size_t count, uint64_t offset) {
  if (offset > INT64_MAX || count > (uint64_t)INT64_MAX - offset)
    return -1;
  size_t used = 0;
  while (used < count) {
    ssize_t n = pread(fd, (char *)output + used, count - used, (off_t)(offset + used));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    used += (size_t)n;
  }
  return 0;
}

int mpc_elf_build_id(int fd, char output[129]) {
  unsigned char ident[EI_NIDENT];
  if (
    read_at(fd, ident, sizeof ident, 0) || memcmp(ident, ELFMAG, SELFMAG) ||
    ident[EI_DATA] != ELFDATA2LSB || ident[EI_VERSION] != EV_CURRENT)
    return -1;
  uint64_t offset;
  unsigned count, size;
  if (ident[EI_CLASS] == ELFCLASS32) {
    Elf32_Ehdr header;
    if (read_at(fd, &header, sizeof header, 0) || header.e_phentsize != sizeof(Elf32_Phdr))
      return -1;
    offset = header.e_phoff;
    count = header.e_phnum;
    size = header.e_phentsize;
  } else if (ident[EI_CLASS] == ELFCLASS64) {
    Elf64_Ehdr header;
    if (read_at(fd, &header, sizeof header, 0) || header.e_phentsize != sizeof(Elf64_Phdr))
      return -1;
    offset = header.e_phoff;
    count = header.e_phnum;
    size = header.e_phentsize;
  } else
    return -1;
  if (
    !count || count > 1024 || offset > INT64_MAX ||
    (uint64_t)count * size > (uint64_t)INT64_MAX - offset)
    return -1;
  size_t budget = 1024 * 1024;
  for (unsigned i = 0; i < count; i++) {
    uint64_t note_offset, note_size;
    uint32_t type;
    if (ident[EI_CLASS] == ELFCLASS32) {
      Elf32_Phdr entry;
      if (read_at(fd, &entry, sizeof entry, offset + (uint64_t)i * size))
        return -1;
      type = entry.p_type;
      note_offset = entry.p_offset;
      note_size = entry.p_filesz;
    } else {
      Elf64_Phdr entry;
      if (read_at(fd, &entry, sizeof entry, offset + (uint64_t)i * size))
        return -1;
      type = entry.p_type;
      note_offset = entry.p_offset;
      note_size = entry.p_filesz;
    }
    if (type != PT_NOTE)
      continue;
    if (note_size > 65536 || note_size > budget)
      return -1;
    budget -= (size_t)note_size;
    unsigned char notes[65536];
    if (read_at(fd, notes, (size_t)note_size, note_offset))
      return -1;
    size_t p = 0;
    while (note_size - p >= sizeof(Elf32_Nhdr)) {
      Elf32_Nhdr header;
      memcpy(&header, notes + p, sizeof header);
      p += sizeof header;
      if (header.n_namesz > note_size - p)
        return -1;
      size_t name_size = ((size_t)header.n_namesz + 3) & ~(size_t)3;
      if (name_size > note_size - p)
        return -1;
      size_t desc = p + name_size;
      if (header.n_descsz > note_size - desc)
        return -1;
      size_t desc_size = ((size_t)header.n_descsz + 3) & ~(size_t)3;
      if (desc_size > note_size - desc)
        return -1;
      if (
        header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4 &&
        !memcmp(notes + p, "GNU\0", 4) && header.n_descsz && header.n_descsz <= 64) {
        for (uint32_t j = 0; j < header.n_descsz; j++)
          sprintf(output + j * 2, "%02x", notes[desc + j]);
        return 0;
      }
      p = desc + desc_size;
    }
  }
  return -1;
}
