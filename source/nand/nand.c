#include "nand.h"
#include "fsdrive.h"
#include "fsutil.h"
#include "platform.h"
#include "keydb.h"
#include "aes.h"
#include "sha.h"
#include "sdmmc.h"
#include "image.h"

#define NAND_MIN_SECTORS ((GetUnitPlatform() == PLATFORM_N3DS) ? 0x26C000 : 0x1D7800)

static u8 slot0x05KeyY[0x10] = { 0x00 }; // need to load this from FIRM0 / external file
static u8 slot0x05KeyY_sha256[0x20] = { // hash for slot0x05KeyY (16 byte)
    0x98, 0x24, 0x27, 0x14, 0x22, 0xB0, 0x6B, 0xF2, 0x10, 0x96, 0x9C, 0x36, 0x42, 0x53, 0x7C, 0x86,
    0x62, 0x22, 0x5C, 0xFD, 0x6F, 0xAE, 0x9B, 0x0A, 0x85, 0xA5, 0xCE, 0x21, 0xAA, 0xB6, 0xC8, 0x4D
};
    
static u8 nand_magic_n3ds[0x60] = { // NCSD NAND header N3DS magic
    0x4E, 0x43, 0x53, 0x44, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x03, 0x03, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x05, 0x00, 0x00, 0x88, 0x05, 0x00, 0x80, 0x01, 0x00, 0x00,
    0x80, 0x89, 0x05, 0x00, 0x00, 0x20, 0x00, 0x00, 0x80, 0xA9, 0x05, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x80, 0xC9, 0x05, 0x00, 0x80, 0xF6, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static u8 nand_magic_o3ds[0x60] = { // NCSD NAND header O3DS magic
    0x4E, 0x43, 0x53, 0x44, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x03, 0x03, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x05, 0x00, 0x00, 0x88, 0x05, 0x00, 0x80, 0x01, 0x00, 0x00,
    0x80, 0x89, 0x05, 0x00, 0x00, 0x20, 0x00, 0x00, 0x80, 0xA9, 0x05, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x80, 0xC9, 0x05, 0x00, 0x80, 0xAE, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static u8 CtrNandCtr[16];
static u8 TwlNandCtr[16];
static u8 OtpSha256[32] = { 0 };

static u32 emunand_base_sector = 0x000000;

bool InitNandCrypto(void)
{   
    // part #0: KeyX / KeyY for secret sector 0x96
    // on a9lh this MUST be run before accessing the SHA register in any other way
    if (CheckA9lh()) { // for a9lh
        // store the current SHA256 from register
        memcpy(OtpSha256, (void*)REG_SHAHASH, 32);
    } else {
        const char* base[] = { INPUT_PATHS };
        char path[64];
        u8 otp[0x100];
        for (u32 i = 0; i < 2 * (sizeof(base)/sizeof(char*)); i++) {
            snprintf(path, 64, "%s/%s", base[i/2], (i%2) ? "otp0x108.bin" : "otp.bin");
            if (FileGetData(path, otp, 0x100, 0) == 0x100) {
                sha_quick(OtpSha256, otp, 0x90, SHA256_MODE);
                break;
            }
        }
    }
        
    // part #1: Get NAND CID, set up TWL/CTR counter
    u32 NandCid[4];
    u8 shasum[32];
    
    sdmmc_get_cid( 1, NandCid);
    sha_quick(shasum, (u8*) NandCid, 16, SHA256_MODE);
    memcpy(CtrNandCtr, shasum, 16);
    sha_quick(shasum, (u8*) NandCid, 16, SHA1_MODE);
    for(u32 i = 0; i < 16; i++) // little endian and reversed order
        TwlNandCtr[i] = shasum[15-i];
    
    // part #2: TWL KEY
    // see: https://www.3dbrew.org/wiki/Memory_layout#ARM9_ITCM
    if (CheckA9lh()) { // only for a9lh
        u32* TwlCustId = (u32*) (0x01FFB808);
        u8 TwlKeyX[16] __attribute__((aligned(32)));
        u8 TwlKeyY[16] __attribute__((aligned(32)));
        
        // thanks b1l1s & Normmatt
        // see source from https://gbatemp.net/threads/release-twltool-dsi-downgrading-save-injection-etc-multitool.393488/
        const char* nintendo = "NINTENDO";
        u32 TwlKeyXW0 = (TwlCustId[0] ^ 0xB358A6AF) | 0x80000000;
        u32 TwlKeyXW3 = TwlCustId[1] ^ 0x08C267B7;
        memcpy(TwlKeyX +  4, nintendo, 8);
        memcpy(TwlKeyX +  0, &TwlKeyXW0, 4);
        memcpy(TwlKeyX + 12, &TwlKeyXW3, 4);
        
        // see: https://www.3dbrew.org/wiki/Memory_layout#ARM9_ITCM
        u32 TwlKeyYW3 = 0xE1A00005;
        memcpy(TwlKeyY, (u8*) 0x01FFD3C8, 12);
        memcpy(TwlKeyY + 12, &TwlKeyYW3, 4);
        
        setup_aeskeyX(0x03, TwlKeyX);
        setup_aeskeyY(0x03, TwlKeyY);
        use_aeskey(0x03);
    }
    
    // part #3: CTRNAND N3DS KEY
    // thanks AuroraWright and Gelex for advice on this
    // see: https://github.com/AuroraWright/Luma3DS/blob/master/source/crypto.c#L347
    if (CheckA9lh()) { // only for a9lh
        u8 ctr[16] __attribute__((aligned(32)));
        u8 keyY[16] __attribute__((aligned(32)));
        u8 header[0x200];
        
        // section 2 header of FIRM0
        // this is @0x066A00 in FIRM90 & FIRM81
        static u32 offsetSection2 = 0x066A00;
        ReadNandSectors(header, 0x58980 + (offsetSection2 / 0x200), 1, 0x06, NAND_SYSNAND);
        memcpy(keyY, header + 0x10, 0x10); // 0x15 keyY
        
        // try FIRM90 & FIRM81 offsets, search for the key
        for (u32 fver = 0; fver < 2; fver++) {
            static u32 offset0x05KeyY[2] = { 0x0EB014, 0x0EB24C };
            u32 offset = offset0x05KeyY[fver];
            u8 sector[0x200];
        
            // sector containing the slot0x05 keyY
            // key is encrypted @0x0EB014 in the FIRM90
            // key is encrypted @0x0EB24C in the FIRM81
            ReadNandSectors(sector, 0x58980 + (offset / 0x200), 1, 0x06, NAND_SYSNAND);
            
            // decrypt the sector, get the key
            memcpy(ctr, header + 0x20, 0x10); // 0x15 counter
            add_ctr(ctr, (offset - (offset % 0x200) - (offsetSection2 + 0x800)) / 16);
            for (u32 i = 0x0; i < 0x200; i += 0x10) {
                setup_aeskeyY(0x15, keyY);
                use_aeskey(0x15);
                set_ctr(ctr);
                aes_decrypt(sector + i, sector + i, 1, AES_CNT_CTRNAND_MODE);
                add_ctr(ctr, 0x1);
            }
            memcpy(slot0x05KeyY, sector + (offset % 0x200), 16);
            
            // check the key
            sha_quick(shasum, slot0x05KeyY, 16, SHA256_MODE);
            if (memcmp(shasum, slot0x05KeyY_sha256, 32) == 0) {
                setup_aeskeyY(0x05, slot0x05KeyY);
                use_aeskey(0x05);
                break;
            }
        }
        
        if ((memcmp(shasum, slot0x05KeyY_sha256, 32) != 0) && // last resort
            (LoadKeyFromFile(slot0x05KeyY, 0x05, 'Y', NULL) != 0)) {};
    }
    
    return true;
}

bool CheckSlot0x05Crypto(void)
{
    // step #1 - check the slot0x05KeyY SHA-256
    u8 shasum[32];
    sha_quick(shasum, slot0x05KeyY, 16, SHA256_MODE);
    if (memcmp(shasum, slot0x05KeyY_sha256, 32) == 0)
        return true;
    
    // step #2 - check actual CTRNAND magic
    const u8 magic[8] = {0xE9, 0x00, 0x00, 0x43, 0x54, 0x52, 0x20, 0x20}; 
    const u32 sector = 0x05CAD7;
    u8 buffer[0x200];
    ReadNandSectors(buffer, sector, 1, 0x05, NAND_SYSNAND);
    if (memcmp(buffer, magic, 8) == 0)
        return true;
    
    // failed if we arrive here
    return false;
}

bool CheckSector0x96Crypto(void)
{
    const u8 zeroes[32] = { 0 };
    return !(memcmp(OtpSha256, zeroes, 32) == 0);
}

bool CheckA9lh(void)
{
    return ((*(vu32*) 0x101401C0) == 0);
}

void CryptNand(u8* buffer, u32 sector, u32 count, u32 keyslot)
{
    u32 mode = (sector >= (0x0B100000 / 0x200)) ? AES_CNT_CTRNAND_MODE : AES_CNT_TWLNAND_MODE;
    u8 ctr[16] __attribute__((aligned(32)));
    u32 blocks = count * (0x200 / 0x10);
    
    // copy NAND CTR and increment it
    memcpy(ctr, (sector >= (0x0B100000 / 0x200)) ? CtrNandCtr : TwlNandCtr, 16);
    add_ctr(ctr, sector * (0x200/0x10));
    
    // decrypt the data
    use_aeskey(keyslot);
    ctr_decrypt((void*) buffer, (void*) buffer, blocks, mode, ctr);
}

void CryptSector0x96(u8* buffer, bool encrypt)
{
    u32 mode = encrypt ? AES_CNT_ECB_ENCRYPT_MODE : AES_CNT_ECB_DECRYPT_MODE;
    
    // setup the key
    setup_aeskeyX(0x11, OtpSha256);
    setup_aeskeyY(0x11, OtpSha256 + 16);
    
    // decrypt the sector
    use_aeskey(0x11);
    for (u32 b = 0x0; b < 0x200; b += 0x10, buffer += 0x10)
        aes_decrypt((void*) buffer, (void*) buffer, 1, mode);
}

int ReadNandBytes(u8* buffer, u32 offset, u32 count, u32 keyslot, u32 nand_src)
{
    if (!(offset % 0x200) && !(count % 0x200)) { // aligned data -> simple case 
        // simple wrapper function for ReadNandSectors(...)
        return ReadNandSectors(buffer, offset / 0x200, count / 0x200, keyslot, nand_src);
    } else { // misaligned data -> -___-
        u8 l_buffer[0x200];
        int errorcode = 0;
        if (offset % 0x200) { // handle misaligned offset
            u32 offset_fix = 0x200 - (offset % 0x200);
            errorcode = ReadNandSectors(l_buffer, offset / 0x200, 1, keyslot, nand_src);
            if (errorcode != 0) return errorcode;
            memcpy(buffer, l_buffer + 0x200 - offset_fix, min(offset_fix, count));
            if (count <= offset_fix) return 0;
            offset += offset_fix;
            buffer += offset_fix;
            count -= offset_fix;
        } // offset is now aligned and part of the data is read
        if (count >= 0x200) { // otherwise this is misaligned and will be handled below
            errorcode = ReadNandSectors(buffer, offset / 0x200, count / 0x200, keyslot, nand_src);
            if (errorcode != 0) return errorcode;
        }
        if (count % 0x200) { // handle misaligned count
            u32 count_fix = count % 0x200;
            errorcode = ReadNandSectors(l_buffer, (offset + count) / 0x200, 1, keyslot, nand_src);
            if (errorcode != 0) return errorcode;
            memcpy(buffer + count - count_fix, l_buffer, count_fix);
        }
        return errorcode;
    }
}

int WriteNandBytes(const u8* buffer, u32 offset, u32 count, u32 keyslot, u32 nand_dst)
{
    if (!(offset % 0x200) && !(count % 0x200)) { // aligned data -> simple case 
        // simple wrapper function for WriteNandSectors(...)
        return WriteNandSectors(buffer, offset / 0x200, count / 0x200, keyslot, nand_dst);
    } else { // misaligned data -> -___-
        u8 l_buffer[0x200];
        int errorcode = 0;
        if (offset % 0x200) { // handle misaligned offset
            u32 offset_fix = 0x200 - (offset % 0x200);
            errorcode = ReadNandSectors(l_buffer, offset / 0x200, 1, keyslot, nand_dst);
            if (errorcode != 0) return errorcode;
            memcpy(l_buffer + 0x200 - offset_fix, buffer, min(offset_fix, count));
            errorcode = WriteNandSectors((const u8*) l_buffer, offset / 0x200, 1, keyslot, nand_dst);
            if (errorcode != 0) return errorcode;
            if (count <= offset_fix) return 0;
            offset += offset_fix;
            buffer += offset_fix;
            count -= offset_fix;
        } // offset is now aligned and part of the data is written
        if (count >= 0x200) { // otherwise this is misaligned and will be handled below
            errorcode = WriteNandSectors(buffer, offset / 0x200, count / 0x200, keyslot, nand_dst);
            if (errorcode != 0) return errorcode;
        }
        if (count % 0x200) { // handle misaligned count
            u32 count_fix = count % 0x200;
            errorcode = ReadNandSectors(l_buffer, (offset + count) / 0x200, 1, keyslot, nand_dst);
            if (errorcode != 0) return errorcode;
            memcpy(l_buffer, buffer + count - count_fix, count_fix);
            errorcode = WriteNandSectors((const u8*) l_buffer, (offset + count) / 0x200, 1, keyslot, nand_dst);
            if (errorcode != 0) return errorcode;
        }
        return errorcode;
    }
}

int ReadNandSectors(u8* buffer, u32 sector, u32 count, u32 keyslot, u32 nand_src)
{
    if (!count) return 0; // <--- just to be safe
    if (nand_src == NAND_EMUNAND) { // EmuNAND
        int errorcode = 0;
        if ((sector == 0) && (emunand_base_sector % 0x200000 == 0)) { // GW EmuNAND header handling
            errorcode = sdmmc_sdcard_readsectors(emunand_base_sector + getMMCDevice(0)->total_size, 1, buffer);
            if ((keyslot < 0x40) && (keyslot != 0x11) && !errorcode) CryptNand(buffer, 0, 1, keyslot);
            sector = 1;
            count--;
            buffer += 0x200;
        }
        errorcode = (!errorcode && count) ? sdmmc_sdcard_readsectors(emunand_base_sector + sector, count, buffer) : errorcode;
        if (errorcode) return errorcode;
    } else if (nand_src == NAND_IMGNAND) { // ImgNAND
        int errorcode = ReadImageSectors(buffer, sector, count);
        if (errorcode) return errorcode;
    } else if (nand_src == NAND_SYSNAND) { // SysNAND
        int errorcode = sdmmc_nand_readsectors(sector, count, buffer);
        if (errorcode) return errorcode;   
    } else if (nand_src == NAND_ZERONAND) { // zero NAND (good for XORpads)
        memset(buffer, 0, count * 0x200);
    } else {
        return -1;
    }
    if ((keyslot == 0x11) && (sector == 0x96)) CryptSector0x96(buffer, false);
    else if (keyslot < 0x40) CryptNand(buffer, sector, count, keyslot);
    
    return 0;
}

int WriteNandSectors(const u8* buffer, u32 sector, u32 count, u32 keyslot, u32 nand_dst)
{
    // buffer must not be changed, so this is a little complicated
    for (u32 s = 0; s < count; s += (NAND_BUFFER_SIZE / 0x200)) {
        u32 pcount = min((NAND_BUFFER_SIZE/0x200), (count - s));
        memcpy(NAND_BUFFER, buffer + (s*0x200), pcount * 0x200);
        if ((keyslot == 0x11) && (sector == 0x96)) CryptSector0x96(NAND_BUFFER, true);
        else if (keyslot < 0x40) CryptNand(NAND_BUFFER, sector + s, pcount, keyslot);
        if (nand_dst == NAND_EMUNAND) {
            int errorcode = 0;
            if ((sector + s == 0) && (emunand_base_sector % 0x200000 == 0)) { // GW EmuNAND header handling
                errorcode = sdmmc_sdcard_writesectors(emunand_base_sector + getMMCDevice(0)->total_size, 1, NAND_BUFFER);
                errorcode = (!errorcode && (pcount > 1)) ? sdmmc_sdcard_writesectors(emunand_base_sector + 1, pcount - 1, NAND_BUFFER + 0x200) : errorcode;
            } else errorcode = sdmmc_sdcard_writesectors(emunand_base_sector + sector + s, pcount, NAND_BUFFER);
            if (errorcode) return errorcode;
        } else if (nand_dst == NAND_IMGNAND) {
            int errorcode = WriteImageSectors(NAND_BUFFER, sector + s, pcount);
            if (errorcode) return errorcode;
        } else if (nand_dst == NAND_SYSNAND) {
            int errorcode = sdmmc_nand_writesectors(sector + s, pcount, NAND_BUFFER);
            if (errorcode) return errorcode;
        } else {
            return -1;
        }
    }
    
    return 0;
}

u32 CheckNandType(u32 nand_src)
{
    if (ReadNandSectors(NAND_BUFFER, 0, 1, 0xFF, nand_src) != 0)
        return 0;
    if (memcmp(NAND_BUFFER + 0x100, nand_magic_n3ds, 0x60) == 0) {
        return (GetUnitPlatform() == PLATFORM_3DS) ? 0 : NAND_TYPE_N3DS;
    } else if (memcmp(NAND_BUFFER + 0x100, nand_magic_o3ds, 0x60) == 0) {
        u8 magic[8] = {0xE9, 0x00, 0x00, 0x43, 0x54, 0x52, 0x20, 0x20};
        if (ReadNandSectors(NAND_BUFFER, 0x5CAE5, 1, 0x04, nand_src) != 0)
            return 0;
        return ((GetUnitPlatform() == PLATFORM_3DS) || (memcmp(magic, NAND_BUFFER, 8) == 0)) ?
            NAND_TYPE_O3DS : NAND_TYPE_NO3DS;
    }
    
    return 0;
}

u64 GetNandSizeSectors(u32 nand_src)
{
    u32 sysnand_sectors = getMMCDevice(0)->total_size;
    if (nand_src == NAND_EMUNAND) { // for EmuNAND
        u32 partition_offset = GetPartitionOffsetSector("0:");
        u32 emunand_max_sectors = (partition_offset >= (emunand_base_sector + 1)) ? // +1 for safety
            partition_offset - (emunand_base_sector + 1) : 0; 
        u32 emunand_min_sectors = (emunand_base_sector % 0x200000 == 0) ? sysnand_sectors : NAND_MIN_SECTORS;
        if (emunand_max_sectors >= sysnand_sectors) return sysnand_sectors;
        else return (emunand_min_sectors > emunand_max_sectors) ? 0 : emunand_min_sectors;
    } else if (nand_src == NAND_IMGNAND) { // for images
        u32 img_sectors = (GetMountState() == IMG_NAND) ? GetMountSize() / 0x200 : 0;
        return (img_sectors >= sysnand_sectors) ? sysnand_sectors : (img_sectors >= NAND_MIN_SECTORS) ? NAND_MIN_SECTORS : 0;
    } else if (nand_src == NAND_SYSNAND) { // for SysNAND
        return sysnand_sectors;
    }
    
    return 0;
}

bool InitEmuNandBase(void)
{
    emunand_base_sector = 0x000000; // GW type EmuNAND
    if (CheckNandType(NAND_EMUNAND))
        return true;
    
    emunand_base_sector = 0x000001; // RedNAND type EmuNAND
    if (CheckNandType(NAND_EMUNAND))
        return true;
    
    return false;
}
