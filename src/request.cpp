/**
 * @file request.cpp
 * @brief Generic request interface
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/request.h"
#include "mega/command.h"
#include "mega/logging.h"
#include "mega/megaclient.h"

namespace mega {

bool Request::isFetchNodes() const
{
    return cmds.size() == 1 && dynamic_cast<CommandFetchNodes*>(cmds.back());
}

void Request::add(Command* c)
{
    cmds.push_back(c);
}

size_t Request::size() const
{
    return cmds.size();
}

void Request::get(string* req, bool& suppressSID, MegaClient* client) const
{
    // concatenate all command objects, resulting in an API request
    *req = "[";

    suppressSID = true; // only if all commands in batch are suppressSID

    for (int i = 0; i < (int)cmds.size(); i++)
    {
        req->append(i ? ",{" : "{");
        req->append(cmds[i]->getJSON(client));
        req->append("}");
        suppressSID = suppressSID && cmds[i]->suppressSID;
    }

    req->append("]");
}

bool Request::processCmdJSON(Command* cmd, bool couldBeError)
{
    Error e;
    if (couldBeError && cmd->checkError(e, cmd->client->json))
    {
        return cmd->procresult(Command::Result(Command::CmdError, e));
    }
    else if (cmd->client->json.enterobject())
    {
        return cmd->procresult(Command::CmdObject) && cmd->client->json.leaveobject();
    }
    else if (cmd->client->json.enterarray())
    {
        return cmd->procresult(Command::CmdArray) && cmd->client->json.leavearray();
    }
    else
    {
        return cmd->procresult(Command::CmdItem);
    }
}

bool Request::processSeqTag(Command* cmd, bool withJSON, bool& parsedOk, bool inSeqTagArray)
{
    string st;
    cmd->client->json.storeobject(&st);

    if (inSeqTagArray)
    {
        if (*cmd->client->json.pos == ',') ++cmd->client->json.pos;
    }

    if (cmd->client->mCurrentSeqtag == st ||
        !cmd->client->scsn.ready())  // if we have not started the sc channel, we won't receive the matching st, so ignore st until we do
    {
        cmd->client->mCurrentSeqtag.clear();
        cmd->client->mCurrentSeqtagSeen = false;
        parsedOk = withJSON ? processCmdJSON(cmd, false)
                            : cmd->procresult(Command::Result(Command::CmdError, API_OK)); // just an `st` returned is implicitly successful
        return true;
    }
    else
    {
        // result processing paused until we encounter and process actionpackets matching client->mCurrentSeqtag
        cmd->client->mPriorSeqTag = cmd->client->mCurrentSeqtag;
        cmd->client->mCurrentSeqtag = st;
        cmd->client->mCurrentSeqtagSeen = false;
        cmd->client->mCurrentSeqtagCmdtag = cmd->tag;
        cmd->client->json.pos = nullptr;
        assert(cmd->client->mPriorSeqTag.size() < cmd->client->mCurrentSeqtag.size() ||
              (cmd->client->mPriorSeqTag.size() == cmd->client->mCurrentSeqtag.size() &&
               cmd->client->mPriorSeqTag < cmd->client->mCurrentSeqtag));
        return false;
    }
}

void Request::process(MegaClient* client)
{
    TransferDbCommitter committer(client->tctable);
    client->mTctableRequestCommitter = &committer;

    client->json = json;
    for (; processindex < cmds.size() && !stopProcessing; processindex++)
    {
        Command* cmd = cmds[processindex];

        client->restag = cmd->tag;

        cmd->client = client;

        auto cmdJSON = client->json;
        bool parsedOk = true;

        if (*client->json.pos == ',') ++client->json.pos;

        if (cmd->mSeqtagArray && client->json.enterarray())
        {
            // Some commands need to return seqtag and also some JSON,
            // in which case they are in an array with `st` first, and the JSON second
            // Some commands might or might not produce `st`.  And might return a string.
            // So in the case of success with a string return, but no `st`, the array is [0, "returnValue"]
            // If the command failed, there is no array, just the error code
            assert(cmd->mV3);
            assert(*client->json.pos == '0' || *client->json.pos == '\"');
            if (*client->json.pos == '0' && *(client->json.pos+1) == ',')
            {
                client->json.pos += 2;
                parsedOk = processCmdJSON(cmd, false);
            }
            else if (!processSeqTag(cmd, true, parsedOk, true)) // executes the command's procresult if we match the seqtag
            {
                // we need to wait for sc processing to catch up with the seqtag we just read
                json = cmdJSON;
                return;
            }

            if (parsedOk && !client->json.leavearray())
            {
                LOG_err << "Invalid seqtag array";
                parsedOk = false;
            }
        }
        else if (mV3 && *client->json.pos == '"')
        {
            // For v3 commands, a string result is a string which is a seqtag.
            if (!processSeqTag(cmd, false, parsedOk, false))
            {
                // we need to wait for sc processing to catch up with the seqtag we just read
                json = cmdJSON;
                return;
            }
        }
        else
        {
            // straightforward case - plain JSON response, no seqtag
            parsedOk = processCmdJSON(cmd, true);
        }

        if (!parsedOk)
        {
            LOG_err << "JSON for that command was not recognised/consumed properly, adjusting";
            client->json = cmdJSON;
            client->json.storeobject();
        }
        else
        {
#ifdef DEBUG
            // double check the command consumed the right amount of JSON
            cmdJSON.storeobject();
            if (client->json.pos != cmdJSON.pos)
            {
                assert(client->json.pos == cmdJSON.pos);
            }
#endif
        }
    }

    json = client->json;
    client->json.pos = nullptr;
    if (processindex == cmds.size() || stopProcessing)
    {
        clear();
    }
    client->mTctableRequestCommitter = nullptr;
}

Command* Request::getCurrentCommand()
{
    assert(processindex < cmds.size());
    return cmds[processindex];
}

void Request::serverresponse(std::string&& movestring, MegaClient* client)
{
    assert(processindex == 0);
    jsonresponse = std::move(movestring);
    json.begin(jsonresponse.c_str());

    if (!json.enterarray())
    {
        LOG_err << "Invalid response from server";
    }
}

void Request::servererror(const std::string& e, MegaClient* client)
{
    ostringstream s;

    s << "[";
    for (size_t i = cmds.size(); i--; )
    {
        s << e << (i ? "," : "");
    }
    s << "]";
    serverresponse(s.str(), client);
}

void Request::clear()
{
    for (int i = (int)cmds.size(); i--; )
    {
        if (!cmds[i]->persistent)
        {
            delete cmds[i];
        }
    }
    cmds.clear();
    jsonresponse.clear();
    json.pos = NULL;
    processindex = 0;
    stopProcessing = false;
}

bool Request::empty() const
{
    return cmds.empty();
}

void Request::swap(Request& r)
{
    // we use swap to move between queues, but process only after it gets into the completedreqs
    cmds.swap(r.cmds);
    std::swap(mV3, r.mV3);

    // Although swap would usually swap all fields, these must be empty anyway
    // If swap was used when these were active, we would be moving needed info out of the request-in-progress
    assert(jsonresponse.empty() && r.jsonresponse.empty());
    assert(json.pos == NULL && r.json.pos == NULL);
    assert(processindex == 0 && r.processindex == 0);
}

RequestDispatcher::RequestDispatcher()
{
    nextreqs.push_back(Request());
}

#if defined(MEGA_MEASURE_CODE) || defined(DEBUG)
void RequestDispatcher::sendDeferred()
{
    if (!nextreqs.back().empty())
    {
        LOG_debug << "sending deferred requests";
        nextreqs.push_back(Request());
    }
    nextreqs.back().swap(deferredRequests);
}
#endif

void RequestDispatcher::add(Command *c)
{
#if defined(MEGA_MEASURE_CODE) || defined(DEBUG)
    if (deferRequests && deferRequests(c))
    {
        LOG_debug << "deferring request";
        deferredRequests.add(c);
        return;
    }
#endif

    if (nextreqs.back().size() >= MAX_COMMANDS)
    {
        LOG_debug << "Starting an additional Request due to MAX_COMMANDS";
        nextreqs.push_back(Request());
    }
    if (c->batchSeparately && !nextreqs.back().empty())
    {
        LOG_debug << "Starting an additional Request for a batch-separately command";
        nextreqs.push_back(Request());
    }

    if (!nextreqs.back().empty() && nextreqs.back().mV3 != c->mV3)
    {
        LOG_debug << "Starting an additional Request for v3 transition " << c->mV3;
        nextreqs.push_back(Request());
    }
    if (nextreqs.back().empty())
    {
        nextreqs.back().mV3 = c->mV3;
    }

    nextreqs.back().add(c);
    if (c->batchSeparately)
    {
        nextreqs.push_back(Request());
    }
}

bool RequestDispatcher::cmdspending() const
{
    return !nextreqs.front().empty();
}

bool RequestDispatcher::cmdsInflight() const
{
    return !inflightreq.empty();
}

Command* RequestDispatcher::getCurrentCommand(bool currSeqtagSeen)
{
    return currSeqtagSeen ? inflightreq.getCurrentCommand() : nullptr;
}

void RequestDispatcher::serverrequest(string *out, bool& suppressSID, bool &includesFetchingNodes, bool& v3, MegaClient* client)
{
    assert(inflightreq.empty());
    inflightreq.swap(nextreqs.front());
    nextreqs.pop_front();
    if (nextreqs.empty())
    {
        nextreqs.push_back(Request());
    }
    inflightreq.get(out, suppressSID, client);
    includesFetchingNodes = inflightreq.isFetchNodes();
    v3 = inflightreq.mV3;
#ifdef MEGA_MEASURE_CODE
    csRequestsSent += inflightreq.size();
    csBatchesSent += 1;
#endif
}

void RequestDispatcher::requeuerequest()
{
#ifdef MEGA_MEASURE_CODE
    csBatchesReceived += 1;
#endif
    assert(!inflightreq.empty());
    if (!nextreqs.front().empty())
    {
        nextreqs.push_front(Request());
    }
    nextreqs.front().swap(inflightreq);
}

void RequestDispatcher::serverresponse(std::string&& movestring, MegaClient *client)
{
    CodeCounter::ScopeTimer ccst(client->performanceStats.csResponseProcessingTime);

#ifdef MEGA_MEASURE_CODE
    csBatchesReceived += 1;
    csRequestsCompleted += inflightreq.size();
#endif
    processing = true;
    inflightreq.serverresponse(std::move(movestring), client);
    inflightreq.process(client);
    processing = false;
    if (clearWhenSafe)
    {
        clear();
    }
}

void RequestDispatcher::servererror(const std::string& e, MegaClient *client)
{
    // notify all the commands in the batch of the failure
    // so that they can deallocate memory, take corrective action etc.
    processing = true;
    inflightreq.servererror(e, client);
    inflightreq.process(client);
    assert(inflightreq.empty());
    processing = false;
    if (clearWhenSafe)
    {
        clear();
    }
}

void RequestDispatcher::continueProcessing(MegaClient* client)
{
    assert(!inflightreq.empty());
    processing = true;
    inflightreq.process(client);
    processing = false;
}

void RequestDispatcher::clear()
{
    if (processing)
    {
        // we are being called from a command that is in progress (eg. logout) - delay wiping the data structure until that call ends.
        clearWhenSafe = true;
        inflightreq.stopProcessing = true;
    }
    else
    {
        inflightreq.clear();
        for (auto& r : nextreqs)
        {
            r.clear();
        }
        nextreqs.clear();
        nextreqs.push_back(Request());
        processing = false;
        clearWhenSafe = false;
    }
}

} // namespace
