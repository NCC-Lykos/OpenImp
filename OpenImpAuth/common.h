// The fields of the badge sign-in tile.

#pragma once
#include "helpers.h"

enum OPENIMP_FIELD_ID
{
    SFI_TILEIMAGE = 0,
    SFI_LABEL = 1,
    SFI_STATUS = 2,
    SFI_NUM_FIELDS = 3,  // keep last: used as the field count
};

struct FIELD_STATE_PAIR
{
    CREDENTIAL_PROVIDER_FIELD_STATE cpfs;
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE cpfis;
};

static const FIELD_STATE_PAIR s_rgFieldStatePairs[] =
{
    { CPFS_DISPLAY_IN_BOTH,          CPFIS_NONE },    // SFI_TILEIMAGE
    { CPFS_DISPLAY_IN_BOTH,          CPFIS_NONE },    // SFI_LABEL
    { CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_NONE },    // SFI_STATUS
};

static const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR s_rgCredProvFieldDescriptors[] =
{
    { SFI_TILEIMAGE, CPFT_TILE_IMAGE, L"Image",  CPFG_CREDENTIAL_PROVIDER_LOGO  },
    { SFI_LABEL,     CPFT_LARGE_TEXT, L"Badge"                                  },
    { SFI_STATUS,    CPFT_SMALL_TEXT, L"Status"                                 },
};
