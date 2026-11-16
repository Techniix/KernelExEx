/*
 * ectest.asl: a battery whose data lives in an embedded controller, for
 * testing KEXACPI.VXD's EC handler (qemu -acpitable file=ectest.aml). QEMU
 * has no EC, so this checks that a silent EC costs little and does not hang.
 * Not shipped. GPLv2.
 */
DefinitionBlock ("", "SSDT", 2, "KEXEX", "ECTEST", 1)
{
    Scope (\_SB)
    {
        Device (EC0)
        {
            Name (_HID, EisaId ("PNP0C09"))
            Name (_GPE, 0x10)
            Name (ECOK, 0)
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x62, 0x62, 0, 1)
                IO (Decode16, 0x66, 0x66, 0, 1)
            })
            Method (_REG, 2)
            {
                If (Arg0 == 3) { ECOK = Arg1 }
            }
            OperationRegion (ECOR, EmbeddedControl, 0, 0xFF)
            Field (ECOR, ByteAcc, Lock, Preserve)
            {
                Offset (0x80),
                BSTA, 8,
                BRAT, 16,
                BRCP, 16,
                BVOL, 16
            }
        }

        Device (BAT2)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 3)
            Method (_STA, 0) { Return (0x1F) }
            Method (_BIF, 0)
            {
                Return (Package (13)
                {
                    0, 30000, 28000, 1, 14400, 1400, 700, 1, 1, "EC-PACK", "7", "LION", "Test"
                })
            }
            Method (_BST, 0)
            {
                If (\_SB.EC0.ECOK)
                {
                    Return (Package (4) { \_SB.EC0.BSTA, \_SB.EC0.BRAT, \_SB.EC0.BRCP, \_SB.EC0.BVOL })
                }
                Return (Package (4) { 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF })
            }
        }
    }
}
