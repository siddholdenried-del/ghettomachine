#ifndef GM_MIDI_HOST_OWNER_H
#define GM_MIDI_HOST_OWNER_H
/* Audio's note owners. Low bits represent separate selected/fixed USB and TRS
 * input; the high nibble counts physical keyboard keys sharing a chord note. */
enum { MH_USB_SEL=1, MH_USB_FIXED=2, MH_TRS_SEL=4, MH_TRS_FIXED=8, MH_PANEL=16 };
static uint8_t mh_notes[3][128];
static uint8_t mh_route[2][32];     /* two-bit remembered channel-1 destination */
static uint8_t mh_usb_arp[3][16];   /* USB-only arpeggio latch inherited on key-up */
static uint32_t mh_panel_keys;
static int mh_route_get(int trs, unsigned note)
{
    return (mh_route[trs][note >> 2] >> ((note & 3u) * 2u)) & 3u;
}
static void mh_route_set(int trs, unsigned note, unsigned tr)
{
    unsigned shift = (note & 3u) * 2u;
    uint8_t *p = &mh_route[trs][note >> 2];
    *p = (uint8_t)((*p & ~(3u << shift)) | tr << shift);
}
static void mh_latch_set(int tr, unsigned note, int on)
{
    uint8_t *p = &mh_usb_arp[tr-1][note >> 3];
    unsigned bit = 1u << (note & 7u);
    *p = (uint8_t)(on ? (*p | bit) : (*p & ~bit));
}
/* returns +1/-1 on the first attack or final release; no re-trigger when the
 * other input or a panel key is already holding the same synth note. */
static int mh_own(int tr, unsigned note, unsigned source, int on)
{
    uint8_t *p = &mh_notes[tr-1][note];
    unsigned was = *p, now = was;
    if (source == MH_PANEL) {
        if (on && (was & 240u) != 240u) now += MH_PANEL;
        if (!on && (was & 240u)) now -= MH_PANEL;
    } else {
        now = on ? was | source : was & ~source;
    }
    *p = (uint8_t)now;
    return was ? (now ? 0 : -1) : (now ? 1 : 0);
}
#endif
