// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2017 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINS_H
#define BITCOIN_COINS_H

#include "compressor.h"
#include "core_memusage.h"
#include "memusage.h"
#include "serialize.h"
#include "uint256.h"
#include "sync.h"

#include <assert.h>
#include <stdint.h>

#include <boost/foreach.hpp>
#include <boost/unordered_map.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

/**
 * A UTXO entry.
 *
 * Serialized format:
 * - VARINT((coinbase ? 1 : 0) | (height << 1))
 * - the non-spent CTxOut (via CTxOutCompressor)
 */
class Coin
{
public:
    //! whether the containing transaction was a coinbase
    bool fCoinBase;

    //! unspent transaction output
    CTxOut out;

    //! at which height the containing transaction was included in the active block chain
    uint32_t nHeight;

    //! construct a Coin from a CTxOut and height/coinbase properties.
    Coin(CTxOut&& outIn, int nHeightIn, bool fCoinBaseIn) : fCoinBase(fCoinBaseIn), out(std::move(outIn)), nHeight(nHeightIn) {}
    Coin(const CTxOut& outIn, int nHeightIn, bool fCoinBaseIn) : fCoinBase(fCoinBaseIn), out(outIn), nHeight(nHeightIn) {}

    void Clear() {
        out.SetNull();
        fCoinBase = false;
        nHeight = 0;
    }

    //! empty constructor
    Coin() : fCoinBase(false), nHeight(0) { }

    bool IsCoinBase() const {
        return fCoinBase;
    }

    template<typename Stream>
    void Serialize(Stream &s) const {
        assert(!IsPruned());
        uint32_t code = nHeight * 2 + fCoinBase;
        ::Serialize(s, VARINT(code));
        ::Serialize(s, CTxOutCompressor(REF(out)));
    }

    template<typename Stream>
    void Unserialize(Stream &s) {
        uint32_t code = 0;
        ::Unserialize(s, VARINT(code));
        nHeight = code >> 1;
        fCoinBase = code & 1;
        ::Unserialize(s, REF(CTxOutCompressor(out)));
    }

    bool IsPruned() const {
        return out.IsNull();
    }

    size_t DynamicMemoryUsage() const {
        return memusage::DynamicUsage(out.scriptPubKey);
    }
};

class CCoins
{
public:
    //! whether transaction is a coinbase
    bool fCoinBase;

    //! unspent transaction outputs; spent outputs are .IsNull(); spent outputs at the end of the array are dropped
    std::vector<CTxOut> vout;

    //! at which height this transaction was included in the active block chain
    int nHeight;

    //! empty constructor
    CCoins() : fCoinBase(false), vout(0), nHeight(0), nVersion(0) { }

    template<typename Stream>
    void Unserialize(Stream &s, int nType, int nVersion) {
        unsigned int nCode = 0;
        // version
        ::Unserialize(s, VARINT(this->nVersion), nType, nVersion);
        // header code
        ::Unserialize(s, VARINT(nCode), nType, nVersion);
        fCoinBase = nCode & 1;
        std::vector<bool> vAvail(2, false);
        vAvail[0] = (nCode & 2) != 0;
        vAvail[1] = (nCode & 4) != 0;
        unsigned int nMaskCode = (nCode / 8) + ((nCode & 6) != 0 ? 0 : 1);
        // spentness bitmask
        while (nMaskCode > 0) {
            unsigned char chAvail = 0;
            ::Unserialize(s, chAvail, nType, nVersion);
            for (unsigned int p = 0; p < 8; p++) {
                bool f = (chAvail & (1 << p)) != 0;
                vAvail.push_back(f);
            }
            if (chAvail != 0)
                nMaskCode--;
        }
        // txouts themself
        vout.assign(vAvail.size(), CTxOut());
        for (unsigned int i = 0; i < vAvail.size(); i++) {
            if (vAvail[i])
                ::Unserialize(s, REF(CTxOutCompressor(vout[i])), nType, nVersion);
        }
        // coinbase height
        ::Unserialize(s, VARINT(nHeight));
    }
};

class SaltedOutpointHasher
{
private:
    uint256 salt;

public:
    SaltedOutpointHasher();

    /**
     * This *must* return size_t. With Boost 1.46 on 32-bit systems the
     * unordered_map will behave unpredictably if the custom hasher returns a
     * uint64_t, resulting in failures when syncing the chain (#4634).
     */
    size_t operator()(const COutPoint& id) const {
        return SipHashUint256Extra(k0, k1, id.hash, id.n);
    }
};

struct CCoinsCacheEntry
{
    Coin coins; // The actual cached data.
    unsigned char flags;

    enum Flags {
        DIRTY = (1 << 0), // This cache entry is potentially different from the version in the parent view.
        FRESH = (1 << 1), // The parent view does not have this entry (or it is pruned).
    };

    CCoinsCacheEntry() : flags(0) {}
    explicit CCoinsCacheEntry(Coin&& coin_) : coins(std::move(coin_)), flags(0) {}
};

typedef std::unordered_map<COutPoint, CCoinsCacheEntry, SaltedOutpointHasher> CCoinsMap;

struct CCoinsStats
{
public:
    CCoinsViewCursor(const uint256 &hashBlockIn): hashBlock(hashBlockIn) {}
    virtual ~CCoinsViewCursor() {}

    virtual bool GetKey(COutPoint &key) const = 0;
    virtual bool GetValue(Coin &coin) const = 0;
    virtual unsigned int GetValueSize() const = 0;

    virtual bool Valid() const = 0;
    virtual void Next() = 0;

    //! Get best block at the time this cursor was created
    const uint256 &GetBestBlock() const { return hashBlock; }
private:
    uint256 hashBlock;
    uint64_t nTransactions;
    uint64_t nTransactionOutputs;
    uint64_t nSerializedSize;
    uint256 hashSerialized;
    CAmount nTotalAmount;

    CCoinsStats() : nHeight(0), nTransactions(0), nTransactionOutputs(0), nSerializedSize(0), nTotalAmount(0) {}
};


/** Abstract view on the open txout dataset. */
class CCoinsView
{
public:

    mutable CCriticalSection cs_utxo;

    //! Retrieve the Coin (unspent transaction output) for a given outpoint.
    virtual bool GetCoins(const COutPoint &outpoint, Coin &coin) const;

    //! Just check whether we have data for a given outpoint.
    //! This may (but cannot always) return true for spent outputs.
    virtual bool HaveCoins(const COutPoint &outpoint) const;

    //! Retrieve the block hash whose state this CCoinsView currently represents
    virtual uint256 GetBestBlock() const;

    //! Do a bulk modification (multiple Coin changes + BestBlock change).
    //! The passed mapCoins can be modified.
    virtual bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, size_t &nChildCachedCoinsUsage);

    //! Calculate statistics about the unspent transaction output set
    virtual bool GetStats(CCoinsStats &stats) const;

    //! As we use CCoinsViews polymorphically, have a virtual destructor
    virtual ~CCoinsView() {}
};


/** CCoinsView backed by another CCoinsView */
class CCoinsViewBacked : public CCoinsView
{
protected:
    CCoinsView *base;

public:
    CCoinsViewBacked(CCoinsView *viewIn);
    bool GetCoins(const COutPoint &outpoint, Coin &coin) const override;
    bool HaveCoins(const COutPoint &outpoint) const override;
    uint256 GetBestBlock() const override;
    void SetBackend(CCoinsView &viewIn);
    bool GetStats(CCoinsStats &stats) const;
    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, size_t &nChildCachedCoinsUsage) override;
    CCoinsViewCursor *Cursor() const override;
    size_t EstimateSize() const override;
};


/** CCoinsView that adds a memory cache for transactions to another CCoinsView */
class CCoinsViewCache : public CCoinsViewBacked
{
protected:
    /**
     * Make mutable so that we can "fill the cache" even from Get-methods
     * declared as "const".
     */
    mutable uint256 hashBlock;
    mutable CCoinsMap cacheCoins;

    /* Cached dynamic memory usage for the inner Coin objects. */
    mutable size_t cachedCoinsUsage;

public:
    CCoinsViewCache(CCoinsView *baseIn);

    // Standard CCoinsView methods
    bool GetCoins(const COutPoint &outpoint, Coin &coin) const;
    bool HaveCoins(const COutPoint &outpoint) const;
    uint256 GetBestBlock() const;
    void SetBestBlock(const uint256 &hashBlock);
    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, size_t &nChildCachedCoinsUsage);

    /**
     * Check if we have the given utxo already loaded in this cache.
     * The semantics are the same as HaveCoins(), but no calls to
     * the backing CCoinsView are made.
     */
    bool HaveCoinsInCache(const COutPoint &outpoint) const;

    /**
     * Return a reference to Coin in the cache, or a pruned one if not found. This is
     * more efficient than GetCoins. Modifications to other cache entries are
     * allowed while accessing the returned pointer.
     */
    const Coin& AccessCoin(const COutPoint &output) const;

    /**
     * Add a coin. Set potential_overwrite to true if a non-pruned version may
     * already exist.
     */
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool potential_overwrite);

    /**
     * Spend a coin. Pass moveto in order to get the deleted data.
     * If no unspent output exists for the passed outpoint, this call
     * has no effect.
     */
    void SpendCoin(const COutPoint &outpoint, Coin* moveto = nullptr);

    /**
     * Push the modifications applied to this cache to its base.
     * Failure to call this method before destruction will cause the changes to be forgotten.
     * If false is returned, the state of this cache (and its backing view) will be undefined.
     */
    bool Flush();

    /** 
     * Remove excess entries from this cache.
     * Entries are trimmed starting from the beginning of the map.  In this way if those entries
     * are needed later they will all be collocated near the the beginning of the leveldb database
     * and will be faster to retrieve.
     */
    void Trim(size_t nTrimSize) const;

    /**
     * Removes the UTXO with the given outpoint from the cache, if it is
     * not modified.
     */
    void Uncache(const COutPoint &outpoint);

    //! Calculate the size of the cache (in number of transaction outputs)
    unsigned int GetCacheSize() const;

    //! Calculate the size of the cache (in bytes)
    size_t DynamicMemoryUsage() const;

    //! Recalculate and Reset the size of cachedCoinsUsage
    size_t ResetCachedCoinUsage() const;

    /**
     * Amount of bitcoins coming in to a transaction
     * Note that lightweight clients may not know anything besides the hash of previous transactions,
     * so may not be able to calculate this.
     *
     * @param[in] tx        transaction for which we are checking input total
     * @return        Sum of value of all inputs (scriptSigs)
     */
    CAmount GetValueIn(const CTransaction& tx) const;

    //! Check whether all prevouts of the transaction are present in the UTXO set represented by this view
    bool HaveInputs(const CTransaction& tx) const;

    /**
     * Return priority of tx at height nHeight. Also calculate the sum of the values of the inputs
     * that are already in the chain.  These are the inputs that will age and increase priority as
     * new blocks are added to the chain.
     */
    double GetPriority(const CTransaction &tx, int nHeight, CAmount &inChainInputValue) const;

    const CTxOut &GetOutputFor(const CTxIn& input) const;

private:
    CCoinsMap::iterator FetchCoins(const COutPoint &outpoint) const;

    /**
     * By making the copy constructor private, we prevent accidentally using it when one intends to create a cache on top of a base cache.
     */
    CCoinsViewCache(const CCoinsViewCache &);
};

//! Utility function to add all of a transaction's outputs to a cache.
// It assumes that overwrites are only possible for coinbase transactions,
// TODO: pass in a boolean to limit these possible overwrites to known
// (pre-BIP34) cases.
void AddCoins(CCoinsViewCache& cache, const CTransaction& tx, int nHeight);

//! Utility function to find any unspent output with a given txid.
const Coin& AccessByTxid(const CCoinsViewCache& cache, const uint256& txid);

#endif // BITCOIN_COINS_H
