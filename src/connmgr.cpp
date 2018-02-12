// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2016-2017 The Bitcoin Unlimited developers

#include "connmgr.h"
#include "requestManager.h"
#include "expedited.h"

std::unique_ptr<CConnMgr> connmgr(new CConnMgr);

/**
 * Find a node in a vector.  Just calls std::find.  Split out for cleanliness and also because std:find won't be
 * enough if we switch to vectors of noderefs.  Requires any lock for vector traversal to be held.
 */
static std::vector<CNode_ptr>::iterator FindNode(std::vector<CNode_ptr> &vNodes, CNode_ptr pnode)
{
    return std::find(vNodes.begin(), vNodes.end(), pnode);
}

/**
 * Add a node to a vector, and take a reference.  This function does NOT prevent adding duplicates.
 * Requires any lock for vector traversal to be held.
 * @param[in] vNodes      The vector of nodes.
 * @param[in] pnode       The node to add.
 */
static void AddNode(std::vector<CNode_ptr> &vNodes, CNode_ptr pnode)
{
    pnode->AddRef();
    vNodes.push_back(pnode);
}

/**
 * If a node is present in a vector, remove it and drop a reference.
 * Requires any lock for vector traversal to be held.
 * @param[in] vNodes      The vector of nodes.
 * @param[in] pnode       The node to remove.
 * @return  True if the node was originally present, false if not.
 */
static bool RemoveNode(std::vector<CNode_ptr> &vNodes, CNode_ptr pnode)
{
    auto Node = FindNode(vNodes, pnode);
    if (Node != vNodes.end())
    {
        pnode->Release();
        vNodes.erase(Node);
        return true;
    }

    return false;
}

CConnMgr::CConnMgr() : nExpeditedBlocksMax(32), nExpeditedTxsMax(32), next(0)
{
    vSendExpeditedBlocks.reserve(256);
    vSendExpeditedTxs.reserve(256);
    vExpeditedUpstream.reserve(256);
}

NodeId CConnMgr::NextNodeId()
{
    // Pre-increment; do not use zero
    return ++next;
}

/**
 * Given a node ID, return a node reference to the node.
 */
CNode_ptr CConnMgr::FindNodeFromId(NodeId id)
{
    LOCK(cs_vNodes);
    for (auto pnode : vNodes)
    {
        if (pnode->GetId() == id)
            return pnode;
    }

    return nullptr;
}

void CConnMgr::EnableExpeditedSends(CNode_ptr pnode, bool fBlocks, bool fTxs, bool fForceIfFull)
{
    LOCK(cs_expedited);

    if (fBlocks && FindNode(vSendExpeditedBlocks, pnode) == vSendExpeditedBlocks.end())
    {
        if (fForceIfFull || vSendExpeditedBlocks.size() < nExpeditedBlocksMax)
        {
            AddNode(vSendExpeditedBlocks, pnode);
            LOG(THIN, "Enabled expedited blocks to peer %s (%u peers total)\n", pnode->GetLogName(),
                vSendExpeditedBlocks.size());
        }
        else
        {
            LOG(THIN, "Cannot enable expedited blocks to peer %s, I am full (%u peers total)\n", pnode->GetLogName(),
                vSendExpeditedBlocks.size());
        }
    }

    if (fTxs && FindNode(vSendExpeditedTxs, pnode) == vSendExpeditedTxs.end())
    {
        if (fForceIfFull || vSendExpeditedTxs.size() < nExpeditedTxsMax)
        {
            AddNode(vSendExpeditedTxs, pnode);
            LOG(THIN, "Enabled expedited txs to peer %s (%u peers total)\n", pnode->GetLogName(),
                vSendExpeditedTxs.size());
        }
        else
        {
            LOG(THIN, "Cannot enable expedited txs to peer %s, I am full (%u peers total)\n", pnode->GetLogName(),
                vSendExpeditedTxs.size());
        }
    }
}

void CConnMgr::DisableExpeditedSends(CNode_ptr pnode, bool fBlocks, bool fTxs)
{
    LOCK(cs_expedited);

    if (fBlocks && RemoveNode(vSendExpeditedBlocks, pnode))
        LOG(THIN, "Disabled expedited blocks to peer %s (%u peers total)\n", pnode->GetLogName(),
            vSendExpeditedBlocks.size());

    if (fTxs && RemoveNode(vSendExpeditedTxs, pnode))
        LOG(THIN, "Disabled expedited txs to peer %s (%u peers total)\n", pnode->GetLogName(),
            vSendExpeditedTxs.size());
}

void CConnMgr::HandleCommandLine()
{
    nExpeditedBlocksMax = GetArg("-maxexpeditedblockrecipients", nExpeditedBlocksMax);
    nExpeditedTxsMax = GetArg("-maxexpeditedtxrecipients", nExpeditedTxsMax);
}

// Called after a node is removed from the node list.
void CConnMgr::RemovedNode(CNode_ptr pnode)
{
    LOCK(cs_expedited);

    RemoveNode(vSendExpeditedBlocks, pnode);
    RemoveNode(vSendExpeditedTxs, pnode);
    RemoveNode(vExpeditedUpstream, pnode);
}

void CConnMgr::ExpeditedNodeCounts(uint32_t &nBlocks, uint32_t &nTxs, uint32_t &nUpstream)
{
    LOCK(cs_expedited);

    nBlocks = vSendExpeditedBlocks.size();
    nTxs = vSendExpeditedTxs.size();
    nUpstream = vExpeditedUpstream.size();
}

VNodeRefs CConnMgr::ExpeditedBlockNodes()
{
    LOCK(cs_expedited);

    VNodeRefs vRefs;

    for (auto pnode : vExpeditedUpstream)
        vRefs.push_back(CNodeRef(pnode));

    return vRefs;
}

bool CConnMgr::PushExpeditedRequest(CNode_ptr pnode, uint64_t flags)
{
    if (!IsThinBlocksEnabled())
        return error("Thinblocks is not enabled so cannot request expedited blocks from peer %s", pnode->GetLogName());

    if (!pnode->ThinBlockCapable())
        return error("Remote peer has not enabled Thinblocks so you cannot request expedited blocks from %s",
            pnode->GetLogName());

    if (flags & EXPEDITED_BLOCKS)
    {
        LOCK(cs_expedited);

        // Add or remove this node as an upstream node
        if (flags & EXPEDITED_STOP)
        {
            RemoveNode(vExpeditedUpstream, pnode);
            LOGA("Requesting a stop of expedited blocks from peer %s\n", pnode->GetLogName());
        }
        else
        {
            if (FindNode(vExpeditedUpstream, pnode) == vExpeditedUpstream.end())
                AddNode(vExpeditedUpstream, pnode);
            LOGA("Requesting expedited blocks from peer %s\n", pnode->GetLogName());
        }
    }

    // Push even if its a repeat to allow the operator to use the CLI or GUI to force another message.
    pnode->PushMessage(NetMsgType::XPEDITEDREQUEST, flags);

    return true;
}

bool CConnMgr::IsExpeditedUpstream(CNode_ptr pnode)
{
    LOCK(cs_expedited);

    return FindNode(vExpeditedUpstream, pnode) != vExpeditedUpstream.end();
}
