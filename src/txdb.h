// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2017 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include "coins.h"
#include "dbwrapper.h"

#include <map>
#include <string>
#include <utility>
#include <vector>


class CBlockFileInfo;
class CBlockIndex;
struct CDiskTxPos;
class uint256;

//! -dbcache default (MiB)
static const int64_t nDefaultDbCache = 500;
//! max. -dbcache in (MiB)
static const int64_t nMaxDbCache = sizeof(void*) > 4 ? 16384 : 2048;
//! min. -dbcache in (MiB)
static const int64_t nMinDbCache = 4;
//! max increase in cache size since the last time we did a full flush
static const int64_t nMaxCacheIncreaseSinceLastFlush = 512 * 1000 * 1000;

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB : public CCoinsView
{
protected:
    CDBWrapper db;


public:
    CCoinsViewDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override;
    bool HaveCoin(const COutPoint &outpoint) const override;
    uint256 GetBestBlock() const override;
    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, size_t &nChildCachedCoinsUsage) override;
    CCoinsViewCursor *Cursor() const override;

    //! Attempt to update from an older database format. Returns whether an error occurred.
    bool Upgrade();
    size_t EstimateSize() const override;
};

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor: public CCoinsViewCursor
{
public:
    ~CCoinsViewDBCursor() {}

    bool GetKey(COutPoint &key) const;
    bool GetValue(Coin &coin) const;
    unsigned int GetValueSize() const;

    bool Valid() const;
    void Next();

private:
    CCoinsViewDBCursor(CDBIterator* pcursorIn, const uint256 &hashBlockIn):
        CCoinsViewCursor(hashBlockIn), pcursor(pcursorIn) {}
    std::unique_ptr<CDBIterator> pcursor;
    std::pair<char, COutPoint> keyTmp;

    friend class CCoinsViewDB;
>>>>>>> 5083079... Switch CCoinsView and chainstate db from per-txid to per-txout
};

/** Access to the block database (blocks/index/) */
class CBlockTreeDB : public CDBWrapper
{
public:
    CBlockTreeDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
private:
    CBlockTreeDB(const CBlockTreeDB&);
    void operator=(const CBlockTreeDB&);
public:
    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo &fileinfo);
    bool ReadLastBlockFile(int &nFile);
    bool WriteReindexing(bool fReindex);
    bool ReadReindexing(bool &fReindex);
    bool ReadTxIndex(const uint256 &txid, CDiskTxPos &pos);
    bool WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> > &list);
    bool WriteFlag(const std::string &name, bool fValue);
    bool ReadFlag(const std::string &name, bool &fValue);
    bool LoadBlockIndexGuts();
};

#endif // BITCOIN_TXDB_H
