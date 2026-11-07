// The opcode matrix used by tools/asm65816.mjs (only the 6502 subset is used by the patch).
// W65C816 opcode matrix. M/X immediate lengths depend on processor status.
export const opcodeMatrix = `
BRK:8 ORA:dxi COP:8 ORA:sr TSB:d ORA:d ASL:d ORA:dl PHP:i ORA:m ASL:A PHD:i TSB:a ORA:a ASL:a ORA:l
BPL:r ORA:diy ORA:di ORA:siy TRB:d ORA:dx ASL:dx ORA:dly CLC:i ORA:ay INC:A TCS:i TRB:a ORA:ax ASL:ax ORA:lx
JSR:a AND:dxi JSL:l AND:sr BIT:d AND:d ROL:d AND:dl PLP:i AND:m ROL:A PLD:i BIT:a AND:a ROL:a AND:l
BMI:r AND:diy AND:di AND:siy BIT:dx AND:dx ROL:dx AND:dly SEC:i AND:ay DEC:A TSC:i BIT:ax AND:ax ROL:ax AND:lx
RTI:i EOR:dxi WDM:8 EOR:sr MVP:b EOR:d LSR:d EOR:dl PHA:i EOR:m LSR:A PHK:i JMP:a EOR:a LSR:a EOR:l
BVC:r EOR:diy EOR:di EOR:siy MVN:b EOR:dx LSR:dx EOR:dly CLI:i EOR:ay PHY:i TCD:i JML:l EOR:ax LSR:ax EOR:lx
RTS:i ADC:dxi PER:rl ADC:sr STZ:d ADC:d ROR:d ADC:dl PLA:i ADC:m ROR:A RTL:i JMP:ai ADC:a ROR:a ADC:l
BVS:r ADC:diy ADC:di ADC:siy STZ:dx ADC:dx ROR:dx ADC:dly SEI:i ADC:ay PLY:i TDC:i JMP:axi ADC:ax ROR:ax ADC:lx
BRA:r STA:dxi BRL:rl STA:sr STY:d STA:d STX:d STA:dl DEY:i BIT:m TXA:i PHB:i STY:a STA:a STX:a STA:l
BCC:r STA:diy STA:di STA:siy STY:dx STA:dx STX:dy STA:dly TYA:i STA:ay TXS:i TXY:i STZ:a STA:ax STZ:ax STA:lx
LDY:x LDA:dxi LDX:x LDA:sr LDY:d LDA:d LDX:d LDA:dl TAY:i LDA:m TAX:i PLB:i LDY:a LDA:a LDX:a LDA:l
BCS:r LDA:diy LDA:di LDA:siy LDY:dx LDA:dx LDX:dy LDA:dly CLV:i LDA:ay TSX:i TYX:i LDY:ax LDA:ax LDX:ay LDA:lx
CPY:x CMP:dxi REP:8 CMP:sr CPY:d CMP:d DEC:d CMP:dl INY:i CMP:m DEX:i WAI:i CPY:a CMP:a DEC:a CMP:l
BNE:r CMP:diy CMP:di CMP:siy PEI:di CMP:dx DEC:dx CMP:dly CLD:i CMP:ay PHX:i STP:i JML:ali CMP:ax DEC:ax CMP:lx
CPX:x SBC:dxi SEP:8 SBC:sr CPX:d SBC:d INC:d SBC:dl INX:i SBC:m NOP:i XBA:i CPX:a SBC:a INC:a SBC:l
BEQ:r SBC:diy SBC:di SBC:siy PEA:16 SBC:dx INC:dx SBC:dly SED:i SBC:ay PLX:i XCE:i JSR:axi SBC:ax INC:ax SBC:lx
`.trim().split(/\s+/).map(x => x.split(':'));
