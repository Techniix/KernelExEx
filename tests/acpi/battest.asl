/*
 * battest.asl: a simulated laptop for testing KEXACPI.VXD in QEMU
 * (qemu -acpitable file=battest.aml). Not shipped. GPLv2.
 *
 * BAT0: a modern battery (_BIX, mW/mWh), discharging; each _BST read takes
 *       500 mWh off, so the charge visibly drops.
 * BAT1: an older one (_BIF only, mA/mAh), charging; each read adds 50 mAh.
 * AC:   adapter, reports "on battery".
 * LID0: lid, open.
 */
DefinitionBlock ("", "SSDT", 2, "KEXEX", "BATTEST", 1)
{
    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 1)
            Name (CAP0, 40000)
            Method (_STA, 0) { Return (0x1F) }
            Method (_BIX, 0)
            {
                Return (Package (21)
                {
                    1,          // revision
                    0,          // power unit: mW
                    50000,      // design capacity
                    45000,      // last full charge
                    1,          // rechargeable
                    11100,      // design voltage
                    2250, 1125, // warning, low
                    123,        // cycle count
                    100000,     // accuracy
                    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                    100, 100,   // granularity
                    "KEX-TEST-6C", "0042", "LION", "KernelExEx",
                    0           // swapping
                })
            }
            Method (_BST, 0)
            {
                If (CAP0 > 1000) { CAP0 -= 500 }
                Return (Package (4) { 1, 9000, CAP0, 11800 })
            }
        }

        Device (BAT1)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 2)
            Name (CAP1, 2000)
            Method (_STA, 0) { Return (0x1F) }
            Method (_BIF, 0)
            {
                Return (Package (13)
                {
                    1, 4800, 4400, 1, 10800, 440, 220, 10, 10,
                    "OLD-PACK", 12345, "NiMH", "Retro"
                })
            }
            Method (_BST, 0)
            {
                If (CAP1 < 4300) { CAP1 += 50 }
                Return (Package (4) { 2, 1500, CAP1, 11000 })
            }
        }

        Device (AC)
        {
            Name (_HID, "ACPI0003")
            Method (_PSR, 0) { Return (0) }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0) { Return (1) }
        }
    }
}
