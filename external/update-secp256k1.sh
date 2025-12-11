#! /bin/sh

REPO="https://github.com/bitcoin-core/secp256k1"
EXTNAME="secp256k1"
COMMIT="0cdc758a56360bf58a851fe91085a327ec97685a"

DIR=`mktemp -d`

if git clone ${REPO} "$DIR"; then
  :
else
  printf 'Failed to clone "%s".\n' "${REPO}" 1>&2
  exit 1
fi

ORIGDIR="`pwd`/external"

cd ${DIR}

if git reset --hard ${COMMIT}; then
  :
else
  printf 'Failed to find commit "%s".\n' "${COMMIT}" 1>&2
  cd "$ORIGDIR"
  rm -rf "${DIR}"
  exit 1
fi

find '(' -name '.git'  ')' -exec rm -rf '{}' ';'

if patch -p1 < "${ORIGDIR}"/"${EXTNAME}.patch"; then
  :
else
  printf 'Failed to apply patch "%s".\n' "${ORIGDIR}"/"${EXTNAME}.patch" 1>&2
  cd "$ORIGDIR"
  rm -rf ${DIR}
  exit 1
fi

cd "$ORIGDIR"
rm -rf "${EXTNAME}" > /dev/null 2>&1
cp -r "${DIR}" "${EXTNAME}"
