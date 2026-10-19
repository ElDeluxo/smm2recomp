"""The known-good Super Mario Maker 2 v3.0.3 dumps: identifiers of the player's own files, never the files.

setup.py checks a dump against these before anything is built from it; the launcher (runtime/ui/launcher.mm) runs
setup.py, and scripts/check_package.py uses the hashes to make sure no release package carries the game's code.

- NSO build ids (module ids) name a build of the code: the recomp's addresses, patches and findings are for v3.0.3's.
- SHA-256 of the extracted ExeFS files: the same for every v3.0.3 dump (NSP, XCI, an emulator's extraction), since the
  update ships the whole ExeFS. Taken from the owner's dump (2026-10-19).
- Title ids, versions and the content ids of the program NCAs (Nintendo's own content hashes, as listed in each
  title's metadata), to name what an NSP / XCI holds before anything is extracted.
"""

TITLE = 'Super Mario Maker 2'
BASE_TITLE_ID = 0x01009B90006DC000
UPDATE_TITLE_ID = 0x01009B90006DC800
# title versions as the titles' metadata gives them (a patch counter: 3.0.3 is the seventh update), and what they are
UPDATE_VERSION = 458752  # 0x70000
TITLE_VERSIONS = {
    (BASE_TITLE_ID, 0): '1.0.0',
    (UPDATE_TITLE_ID, 458752): '3.0.3',
}

# NSO build ids (hex of the 0x20-byte module id, trailing zeros dropped) by version
BUILD_IDS = {
    '3.0.3': {
        'main': 'c2dc405ac414c37c8b1c50219c7a0f0c',
        'sdk': 'fcf93a41b08c5193a1410d262602dc621617c56f',
    },
    # (recognised, to say what's wrong: the base game without the update)
    '1.0.0': {
        'main': '3ebb1eb2fffb029a8b87579b961a7b',
    },
}

# SHA-256 of the v3.0.3 ExeFS files
EXEFS_SHA256 = {
    'main': '8d838d99cd02493f1de1d0a40bb578492d66419c803a724e27054c60dd60e9af',
    'sdk': '754cf4b581d749fbe3d457cf66497b141ef994cf592d247919d64a37dcc30d27',
    'rtld': 'e893e12a4199b0c2c15775bf03f58987fa6227923ba8a04832aabf2352d2b1b7',
    'main.npdm': '8593d003a71a123b49020c8c7c962e937259aca1b8c2bc91e1d70c71c178e7ae',
}

# Program NCA content ids (the first half of the NCA's SHA-256) of the known titles
PROGRAM_NCA_IDS = {
    'c929dda83a4cdb439bcd193074487476': 'base game 1.0.0',
    '1dbc3f023b82689f794610f48c15ae19': 'update 3.0.3',
}

EXEFS_FILES = ('main', 'sdk', 'rtld', 'main.npdm')


def version_of_build_id(module, build_id):
    """'3.0.3', '1.0.0', or None for a build id of another version (or another game)."""
    for ver, ids in BUILD_IDS.items():
        want = ids.get(module)
        if want and build_id and build_id.startswith(want):
            return ver
    return None


def display_version(title_id, v):
    """A title version by its name ('3.0.3'), or its number for an unknown one ('v327680')."""
    return TITLE_VERSIONS.get((title_id, v), f'v{v}')
