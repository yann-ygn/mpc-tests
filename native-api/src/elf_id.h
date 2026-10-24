#ifndef MPC_ELF_ID_H
#define MPC_ELF_ID_H
/* Read the GNU build-ID note through bounded ELF32/ELF64 program headers. */
int mpc_elf_build_id(int fd, char output[129]);
#endif
