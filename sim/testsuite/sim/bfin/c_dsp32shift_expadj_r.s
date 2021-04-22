//Original:/testcases/core/c_dsp32shift_expadj_r/c_dsp32shift_expadj_r.dsp
// Spec Reference: dsp32shift expadj r
# mach: bfin

.include "testutils.inc"
	start



imm32 r0, 0x08000800;
imm32 r1, 0x08000801;
imm32 r2, 0x08000802;
imm32 r3, 0x08000803;
imm32 r4, 0x08000804;
imm32 r5, 0x08000805;
imm32 r6, 0x08000806;
imm32 r7, 0x08000807;
//rl0 = expadj r0 by rl0;
R1.L = EXPADJ( R1 , R0.L );
R2.L = EXPADJ( R2 , R0.L );
R3.L = EXPADJ( R3 , R0.L );
R4.L = EXPADJ( R4 , R0.L );
R5.L = EXPADJ( R5 , R0.L );
R6.L = EXPADJ( R6 , R0.L );
R7.L = EXPADJ( R7 , R0.L );
CHECKREG r0, 0x08000800;
CHECKREG r1, 0x08000800;
CHECKREG r2, 0x08000800;
CHECKREG r3, 0x08000800;
CHECKREG r4, 0x08000800;
CHECKREG r5, 0x08000800;
CHECKREG r6, 0x08000800;
CHECKREG r7, 0x08000800;

imm32 r0, 0x0900d001;
imm32 r1, 0x09000001;
imm32 r2, 0x0900d002;
imm32 r3, 0x0900d003;
imm32 r4, 0x0900d004;
imm32 r5, 0x0900d005;
imm32 r6, 0x0900d006;
imm32 r7, 0x0900d007;
R0.L = EXPADJ( R0 , R1.L );
R1.L = EXPADJ( R1 , R1.L );
R2.L = EXPADJ( R2 , R1.L );
R3.L = EXPADJ( R3 , R1.L );
R4.L = EXPADJ( R4 , R1.L );
R5.L = EXPADJ( R5 , R1.L );
R6.L = EXPADJ( R6 , R1.L );
R7.L = EXPADJ( R7 , R1.L );
CHECKREG r0, 0x09000001;
CHECKREG r1, 0x09000001;
CHECKREG r2, 0x09000001;
CHECKREG r3, 0x09000001;
CHECKREG r4, 0x09000001;
CHECKREG r5, 0x09000001;
CHECKREG r6, 0x09000001;
CHECKREG r7, 0x09000001;


imm32 r0, 0x0a00e001;
imm32 r1, 0x0a00e001;
imm32 r2, 0x0a00000f;
imm32 r3, 0x0a00e003;
imm32 r4, 0x0a00e004;
imm32 r5, 0x0a00e005;
imm32 r6, 0x0a00e006;
imm32 r7, 0x0a00e007;
R0.L = EXPADJ( R0 , R2.L );
R1.L = EXPADJ( R1 , R2.L );
//rl2 = expadj r2 by rl2;
R3.L = EXPADJ( R3 , R2.L );
R4.L = EXPADJ( R4 , R2.L );
R5.L = EXPADJ( R5 , R2.L );
R6.L = EXPADJ( R6 , R2.L );
R7.L = EXPADJ( R7 , R2.L );
CHECKREG r0, 0x0A000003;
CHECKREG r1, 0x0A000003;
CHECKREG r2, 0x0A00000F;
CHECKREG r3, 0x0A000003;
CHECKREG r4, 0x0A000003;
CHECKREG r5, 0x0A000003;
CHECKREG r6, 0x0A000003;
CHECKREG r7, 0x0A000003;

imm32 r0, 0x0b00f001;
imm32 r1, 0x0b00f001;
imm32 r2, 0x0b00f002;
imm32 r3, 0x0b000010;
imm32 r4, 0x0b00f004;
imm32 r5, 0x0b00f005;
imm32 r6, 0x0b00f006;
imm32 r7, 0x0b00f007;
R0.L = EXPADJ( R0 , R3.L );
R1.L = EXPADJ( R1 , R3.L );
R2.L = EXPADJ( R2 , R3.L );
R3.L = EXPADJ( R3 , R3.L );
R4.L = EXPADJ( R4 , R3.L );
R5.L = EXPADJ( R5 , R3.L );
R6.L = EXPADJ( R6 , R3.L );
R7.L = EXPADJ( R7 , R3.L );
CHECKREG r0, 0x0B000003;
CHECKREG r1, 0x0B000003;
CHECKREG r2, 0x0B000003;
CHECKREG r3, 0x0B000003;
CHECKREG r4, 0x0B000003;
CHECKREG r5, 0x0B000003;
CHECKREG r6, 0x0B000003;
CHECKREG r7, 0x0B000003;

imm32 r0, 0x0c0000c0;
imm32 r1, 0x0c0100c0;
imm32 r2, 0x0c0200c0;
imm32 r3, 0x0c0300c0;
imm32 r4, 0x0c0400c0;
imm32 r5, 0x0c0500c0;
imm32 r6, 0x0c0600c0;
imm32 r7, 0x0c0700c0;
R0.L = EXPADJ( R0 , R4.L );
R1.L = EXPADJ( R1 , R4.L );
R2.L = EXPADJ( R2 , R4.L );
R3.L = EXPADJ( R3 , R4.L );
R4.L = EXPADJ( R4 , R4.L );
R5.L = EXPADJ( R5 , R4.L );
R6.L = EXPADJ( R6 , R4.L );
R7.L = EXPADJ( R7 , R4.L );
CHECKREG r0, 0x0C0000C0;
CHECKREG r1, 0x0C0100C0;
CHECKREG r2, 0x0C0200C0;
CHECKREG r3, 0x0C0300C0;
CHECKREG r4, 0x0C0400C0;
CHECKREG r5, 0x0C0500C0;
CHECKREG r6, 0x0C0600C0;
CHECKREG r7, 0x0C0700C0;

imm32 r0, 0xa00100d0;
imm32 r1, 0x000100d1;
imm32 r2, 0xa00200d0;
imm32 r3, 0xa00300d0;
imm32 r4, 0xa00400d0;
imm32 r5, 0xa00500d0;
imm32 r6, 0xa00600d0;
imm32 r7, 0xa00700d0;
R0.L = EXPADJ( R0 , R5.L );
R1.L = EXPADJ( R1 , R5.L );
R2.L = EXPADJ( R2 , R5.L );
R3.L = EXPADJ( R3 , R5.L );
R4.L = EXPADJ( R4 , R5.L );
R5.L = EXPADJ( R5 , R5.L );
R6.L = EXPADJ( R6 , R5.L );
R7.L = EXPADJ( R7 , R5.L );
CHECKREG r0, 0xA0010000;
CHECKREG r1, 0x0001000E;
CHECKREG r2, 0xA0020000;
CHECKREG r3, 0xA0030000;
CHECKREG r4, 0xA0040000;
CHECKREG r5, 0xA0050000;
CHECKREG r6, 0xA0060000;
CHECKREG r7, 0xA0070000;

imm32 r0, 0xb0010000;
imm32 r1, 0xb0010000;
imm32 r2, 0xb002000f;
imm32 r3, 0xb0030000;
imm32 r4, 0xb0040000;
imm32 r5, 0xb0050000;
imm32 r6, 0xb0060000;
imm32 r7, 0xb0070000;
R0.L = EXPADJ( R0 , R6.L );
R1.L = EXPADJ( R1 , R6.L );
R2.L = EXPADJ( R2 , R6.L );
R3.L = EXPADJ( R3 , R6.L );
R4.L = EXPADJ( R4 , R6.L );
R5.L = EXPADJ( R5 , R6.L );
R6.L = EXPADJ( R6 , R6.L );
R7.L = EXPADJ( R7 , R6.L );
CHECKREG r0, 0xB0010000;
CHECKREG r1, 0xB0010000;
CHECKREG r2, 0xB0020000;
CHECKREG r3, 0xB0030000;
CHECKREG r4, 0xB0040000;
CHECKREG r5, 0xB0050000;
CHECKREG r6, 0xB0060000;
CHECKREG r7, 0xB0070000;

imm32 r0, 0xd00100e0;
imm32 r1, 0xd00100e0;
imm32 r2, 0xd00200e0;
imm32 r3, 0xd00300e0;
imm32 r4, 0xd00400e0;
imm32 r5, 0xd00500e0;
imm32 r6, 0xd00600e0;
imm32 r7, 0xd00700e0;
R0.L = EXPADJ( R0 , R7.L );
R1.L = EXPADJ( R1 , R7.L );
R2.L = EXPADJ( R2 , R7.L );
R3.L = EXPADJ( R3 , R7.L );
R4.L = EXPADJ( R4 , R7.L );
R5.L = EXPADJ( R5 , R7.L );
R6.L = EXPADJ( R6 , R7.L );
R7.L = EXPADJ( R7 , R7.L );
CHECKREG r0, 0xD00100E0;
CHECKREG r1, 0xD00100E0;
CHECKREG r2, 0xD00200E0;
CHECKREG r3, 0xD00300E0;
CHECKREG r4, 0xD00400E0;
CHECKREG r5, 0xD00500E0;
CHECKREG r6, 0xD00600E0;
CHECKREG r7, 0xD00700E0;


pass
