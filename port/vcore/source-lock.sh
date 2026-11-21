# Pinned public shadPS4 base and Git tree after applying patches/shadps4-vcore.patch.
# The tree locks both the patched source and the upstream gitlinks for the externals.
VCORE_BASE=f3687cc400d77736b2a7ef1904a1302e17f9441b
VCORE_TREE=778fc542e6555ab7d9b28a2ed701f56ee23066c9

check_vcore_overlay() {
    local source=$1
    [[ $(git -C "$source" rev-parse --verify HEAD 2>/dev/null) == "$VCORE_BASE" ]] &&
        [[ $(git -C "$source" write-tree) == "$VCORE_TREE" ]] &&
        git -C "$source" diff-files --quiet --ignore-submodules=none &&
        [[ -z $(git -C "$source" ls-files --others -- src externals/artswap_at9) ]]
}
