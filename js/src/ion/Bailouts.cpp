/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsinterp.h"
#include "Bailouts.h"
#include "SnapshotReader.h"
#include "Ion.h"
#include "IonCompartment.h"
#include "IonSpewer.h"
#include "jsinfer.h"
#include "jsanalyze.h"
#include "jsinferinlines.h"
#include "IonFrames-inl.h"

using namespace js;
using namespace js::ion;

// These constructor are exactly the same except for the type of the iterator
// which is given to the SnapshotIterator constructor. Doing so avoid the
// creation of virtual functions for the IonIterator but may introduce some
// weirdness as IonInlineIterator is using an IonFrameIterator reference.
//
// If a function relies on ionScript() or to use OsiIndex(), due to the
// lack of virtual, these functions will use the IonFrameIterator reference
// contained in the InlineFrameIterator and thus are not able to recover
// correctly the data stored in IonBailoutIterator.
//
// Currently, such cases should not happen because our only use case of the
// IonFrameIterator within InlineFrameIterator is to read the frame content, or
// to clone it to find the parent scripted frame.  Both use cases are fine and
// should not cause any issue since the only potential issue is to read the
// bailed out frame.

SnapshotIterator::SnapshotIterator(const IonBailoutIterator &iter)
  : SnapshotReader(iter.ionScript()->snapshots() + iter.snapshotOffset(),
                   iter.ionScript()->snapshots() + iter.ionScript()->snapshotsSize()),
    fp_(iter.jsFrame()),
    machine_(iter.machineState()),
    ionScript_(iter.ionScript())
{
}

InlineFrameIterator::InlineFrameIterator(const IonBailoutIterator *iter)
  : frame_(iter),
    framesRead_(0),
    callee_(NULL),
    script_(NULL)
{
    if (iter) {
        start_ = SnapshotIterator(*iter);
        findNextFrame();
    }
}

void
IonBailoutIterator::dump() const
{
    if (type_ == IonFrame_OptimizedJS) {
        InlineFrameIterator frames(this);
        for (;;) {
            frames.dump();
            if (!frames.more())
                break;
            ++frames;
        }
    } else {
        IonFrameIterator::dump();
    }
}

static JSScript*
GetBailedJSScript(JSContext *cx)
{
    AutoAssertNoGC nogc;

    // Just after the frame conversion, we can safely interpret the ionTop as JS
    // frame because it targets the bailed JS frame converted to an exit frame.
    IonJSFrameLayout *frame = reinterpret_cast<IonJSFrameLayout*>(cx->runtime->ionTop);
    switch (GetCalleeTokenTag(frame->calleeToken())) {
      case CalleeToken_Function: {
        JSFunction *fun = CalleeTokenToFunction(frame->calleeToken());
        return fun->nonLazyScript().get(nogc);
      }
      case CalleeToken_Script:
        return CalleeTokenToScript(frame->calleeToken());
      default:
        JS_NOT_REACHED("unexpected callee token kind");
        return NULL;
    }
}

void
StackFrame::initFromBailout(JSContext *cx, SnapshotIterator &iter)
{
    AutoAssertNoGC nogc;
    uint32_t exprStackSlots = iter.slots() - script()->nfixed;

#ifdef TRACK_SNAPSHOTS
    iter.spewBailingFrom();
#endif
    IonSpew(IonSpew_Bailouts, " expr stack slots %u, is function frame %u",
            exprStackSlots, isFunctionFrame());

    if (iter.bailoutKind() == Bailout_ArgumentCheck) {
        // Temporary hack -- skip the (unused) scopeChain, because it could be
        // bogus (we can fail before the scope chain slot is set). Strip the
        // hasScopeChain flag and we'll check this later to run prologue().
        iter.skip();
        flags_ &= ~StackFrame::HAS_SCOPECHAIN;
    } else {
        Value v = iter.read();
        if (v.isObject()) {
            scopeChain_ = &v.toObject();
            flags_ |= StackFrame::HAS_SCOPECHAIN;
            if (isFunctionFrame() && fun()->isHeavyweight())
                flags_ |= StackFrame::HAS_CALL_OBJ;
        } else {
            JS_ASSERT(v.isUndefined());
        }
    }

    // Assume that all new stack frames have had their entry flag set if
    // profiling has been turned on. This will be corrected if necessary
    // elsewhere.
    if (cx->runtime->spsProfiler.enabled())
        setPushedSPSFrame();

    if (isFunctionFrame()) {
        Value thisv = iter.read();
        formals()[-1] = thisv;

        // The new |this| must have already been constructed prior to an Ion
        // constructor running.
        if (isConstructing())
            JS_ASSERT(!thisv.isPrimitive());

        JS_ASSERT(iter.slots() >= CountArgSlots(fun()));
        IonSpew(IonSpew_Bailouts, " frame slots %u, nargs %u, nfixed %u",
                iter.slots(), fun()->nargs, script()->nfixed);

        for (uint32_t i = 0; i < fun()->nargs; i++) {
            Value arg = iter.read();
            formals()[i] = arg;
        }
    }
    exprStackSlots -= CountArgSlots(maybeFun());

    for (uint32_t i = 0; i < script()->nfixed; i++) {
        Value slot = iter.read();
        slots()[i] = slot;
    }

    IonSpew(IonSpew_Bailouts, " pushing %u expression stack slots", exprStackSlots);
    FrameRegs &regs = cx->regs();
    for (uint32_t i = 0; i < exprStackSlots; i++) {
        Value v;

        // If coming from an invalidation bailout, and this is the topmost
        // value, and a value override has been specified, don't read from the
        // iterator. Otherwise, we risk using a garbage value.
        if (!iter.moreFrames() && i == exprStackSlots - 1 && cx->runtime->hasIonReturnOverride())
            v = iter.skip();
        else
            v = iter.read();

        *regs.sp++ = v;
    }
    unsigned pcOff = iter.pcOffset();
    regs.pc = script()->code + pcOff;

    if (iter.resumeAfter())
        regs.pc = GetNextPc(regs.pc);

    IonSpew(IonSpew_Bailouts, " new PC is offset %u within script %p (line %d)",
            pcOff, (void *)script().get(nogc), PCToLineNumber(script().get(nogc), regs.pc));
    JS_ASSERT(exprStackSlots == js_ReconstructStackDepth(cx, script().get(nogc), regs.pc));
}

static StackFrame *
PushInlinedFrame(JSContext *cx, StackFrame *callerFrame)
{
    AssertCanGC();

    // Grab the callee object out of the caller's frame, which has already been restored.
    // N.B. we currently assume that the caller frame is at a JSOP_CALL pc for the caller frames,
    // which will not be the case when we inline getters (in which case it would be a
    // JSOP_GETPROP). That will have to be handled differently.
    FrameRegs &regs = cx->regs();
    JS_ASSERT(JSOp(*regs.pc) == JSOP_CALL || JSOp(*regs.pc) == JSOP_NEW);
    int callerArgc = GET_ARGC(regs.pc);
    const Value &calleeVal = regs.sp[-callerArgc - 2];

    RootedFunction fun(cx, calleeVal.toObject().toFunction());
    RootedScript script(cx, fun->nonLazyScript());
    CallArgs inlineArgs = CallArgsFromSp(callerArgc, regs.sp);

    // Bump the stack pointer to make it look like the inline args have been pushed, but they will
    // really get filled in by RestoreOneFrame.
    regs.sp = inlineArgs.end();

    InitialFrameFlags flags = INITIAL_NONE;
    if (JSOp(*regs.pc) == JSOP_NEW)
        flags = INITIAL_CONSTRUCT;

    if (!cx->stack.pushInlineFrame(cx, regs, inlineArgs, *fun, script, flags, DONT_REPORT_ERROR))
        return NULL;

    StackFrame *fp = cx->stack.fp();
    JS_ASSERT(fp == regs.fp());
    JS_ASSERT(fp->prev() == callerFrame);

    fp->formals()[-2].setObject(*fun);

    return fp;
}

static uint32_t
ConvertFrames(JSContext *cx, IonActivation *activation, IonBailoutIterator &it)
{
    AssertCanGC();
    IonSpew(IonSpew_Bailouts, "Bailing out %s:%u, IonScript %p",
            it.script()->filename, it.script()->lineno, (void *) it.ionScript());
    IonSpew(IonSpew_Bailouts, " reading from snapshot offset %u size %u",
            it.snapshotOffset(), it.ionScript()->snapshotsSize());
#ifdef DEBUG
    // Use count is reset after invalidation. Log use count on bailouts to
    // determine if we have a critical sequence of bailout.
    if (it.script()->ion == it.ionScript()) {
        IonSpew(IonSpew_Bailouts, " Current script use count is %u",
                it.script()->getUseCount());
    }
#endif

    SnapshotIterator iter(it);

    // Set a flag to avoid bailing out on every iteration or function call. Ion can
    // compile and run the script again after an invalidation.
    it.ionScript()->setBailoutExpected();

    // We use OffTheBooks instead of cx because at this time we cannot iterate
    // on the stack safely and the reported error attempts to walk the IonMonkey
    // frames. We cannot iterate on the stack because we have no exit frame to
    // start Ion frames iteratons.
    BailoutClosure *br = js_new<BailoutClosure>();
    if (!br)
        return BAILOUT_RETURN_FATAL_ERROR;
    activation->setBailout(br);

    StackFrame *fp;
    if (it.isEntryJSFrame() && cx->fp()->runningInIon() && activation->entryfp()) {
        // Avoid creating duplicate interpreter frames. This is necessary to
        // avoid blowing out the interpreter stack, and must be used in
        // conjunction with inline-OSR from within bailouts (since each Ion
        // activation must be tied to a unique JSStackFrame for StackIter to
        // work).
        //
        // Note: If the entry frame is a placeholder (a stub frame pushed for
        // JM -> Ion calls), then we cannot re-use it as it does not have
        // enough slots.
        JS_ASSERT(cx->fp() == activation->entryfp());
        fp = cx->fp();
        cx->regs().sp = fp->base();
    } else {
        br->constructFrame();
        if (!cx->stack.pushBailoutArgs(cx, it, br->argsGuard()))
            return BAILOUT_RETURN_FATAL_ERROR;

        fp = cx->stack.pushBailoutFrame(cx, it, *br->argsGuard(), br->frameGuard());
    }

    if (!fp)
        return BAILOUT_RETURN_OVERRECURSED;

    br->setEntryFrame(fp);

    JSFunction *callee = it.maybeCallee();
    if (callee)
        fp->formals()[-2].setObject(*callee);

    if (it.isConstructing())
        fp->setConstructing();

    while (true) {
        IonSpew(IonSpew_Bailouts, " restoring frame");
        fp->initFromBailout(cx, iter);

        if (!iter.moreFrames())
             break;
        iter.nextFrame();

        fp = PushInlinedFrame(cx, fp);
        if (!fp)
            return BAILOUT_RETURN_OVERRECURSED;
    }

    jsbytecode *bailoutPc = fp->script()->code + iter.pcOffset();
    br->setBailoutPc(bailoutPc);

    switch (iter.bailoutKind()) {
      case Bailout_Normal:
        return BAILOUT_RETURN_OK;
      case Bailout_TypeBarrier:
        return BAILOUT_RETURN_TYPE_BARRIER;
      case Bailout_Monitor:
        return BAILOUT_RETURN_MONITOR;
      case Bailout_RecompileCheck:
        return BAILOUT_RETURN_RECOMPILE_CHECK;
      case Bailout_BoundsCheck:
        return BAILOUT_RETURN_BOUNDS_CHECK;
      case Bailout_ShapeGuard:
        return BAILOUT_RETURN_SHAPE_GUARD;
      case Bailout_CachedShapeGuard:
        return BAILOUT_RETURN_CACHED_SHAPE_GUARD;

      // When bailing out from an argument check, none of the code of the
      // function has run yet. When profiling, this means that the function
      // hasn't flagged its entry just yet. It has been "entered," however, so
      // we flag it here manually that the entry has happened.
      case Bailout_ArgumentCheck:
        fp->unsetPushedSPSFrame();
        Probes::enterScript(cx, fp->script().unsafeGet(), fp->script()->function(), fp);
        return BAILOUT_RETURN_ARGUMENT_CHECK;
    }

    JS_NOT_REACHED("bad bailout kind");
    return BAILOUT_RETURN_FATAL_ERROR;
}

static inline void
EnsureExitFrame(IonCommonFrameLayout *frame)
{
    if (frame->prevType() == IonFrame_Entry) {
        // The previous frame type is the entry frame, so there's no actual
        // need for an exit frame.
        return;
    }

    if (frame->prevType() == IonFrame_Rectifier) {
        // The rectifier code uses the frame descriptor to discard its stack,
        // so modifying its descriptor size here would be dangerous. Instead,
        // we change the frame type, and teach the stack walking code how to
        // deal with this edge case. bug 717297 would obviate the need
        frame->changePrevType(IonFrame_Bailed_Rectifier);
        return;
    }

    JS_ASSERT(frame->prevType() == IonFrame_OptimizedJS);
    frame->changePrevType(IonFrame_Bailed_JS);
}

uint32_t
ion::Bailout(BailoutStack *sp)
{
    AssertCanGC();
    JSContext *cx = GetIonContext()->cx;
    // We don't have an exit frame.
    cx->runtime->ionTop = NULL;
    IonActivationIterator ionActivations(cx);
    IonBailoutIterator iter(ionActivations, sp);
    IonActivation *activation = ionActivations.activation();

    // IonCompartment *ioncompartment = cx->compartment->ionCompartment();
    // IonActivation *activation = cx->runtime->ionActivation;
    // FrameRecovery in = FrameRecoveryFromBailout(ioncompartment, sp);

    IonSpew(IonSpew_Bailouts, "Took bailout! Snapshot offset: %d", iter.snapshotOffset());

    uint32_t retval = ConvertFrames(cx, activation, iter);

    EnsureExitFrame(iter.jsFrame());
    return retval;
}

uint32_t
ion::InvalidationBailout(InvalidationBailoutStack *sp, size_t *frameSizeOut)
{
    AssertCanGC();
    sp->checkInvariants();

    JSContext *cx = GetIonContext()->cx;

    // We don't have an exit frame.
    cx->runtime->ionTop = NULL;
    IonActivationIterator ionActivations(cx);
    IonBailoutIterator iter(ionActivations, sp);
    IonActivation *activation = ionActivations.activation();

    IonSpew(IonSpew_Bailouts, "Took invalidation bailout! Snapshot offset: %d", iter.snapshotOffset());

    // Note: the frame size must be computed before we return from this function.
    *frameSizeOut = iter.topFrameSize();

    uint32_t retval = ConvertFrames(cx, activation, iter);

    {
        IonJSFrameLayout *frame = iter.jsFrame();
        IonSpew(IonSpew_Invalidate, "converting to exit frame");
        IonSpew(IonSpew_Invalidate, "   orig calleeToken %p", (void *) frame->calleeToken());
        IonSpew(IonSpew_Invalidate, "   orig frameSize %u", unsigned(frame->prevFrameLocalSize()));
        IonSpew(IonSpew_Invalidate, "   orig ra %p", (void *) frame->returnAddress());

        frame->replaceCalleeToken(NULL);
        EnsureExitFrame(frame);

        IonSpew(IonSpew_Invalidate, "   new  calleeToken %p", (void *) frame->calleeToken());
        IonSpew(IonSpew_Invalidate, "   new  frameSize %u", unsigned(frame->prevFrameLocalSize()));
        IonSpew(IonSpew_Invalidate, "   new  ra %p", (void *) frame->returnAddress());
    }

    iter.ionScript()->decref(cx->runtime->defaultFreeOp());

    if (cx->runtime->hasIonReturnOverride())
        cx->regs().sp[-1] = cx->runtime->takeIonReturnOverride();

    if (retval != BAILOUT_RETURN_FATAL_ERROR) {
        if (activation->entryfp()) {
            if (void *annotation = activation->entryfp()->annotation()) {
                // If the entry frame has an annotation, then we invalidated and have
                // immediately returned into this bailout. Transfer the annotation to
                // the new topmost frame.
                activation->entryfp()->setAnnotation(NULL);
                cx->fp()->setAnnotation(annotation);
            }
        }

        // If invalidation was triggered inside a stub call, we may still have to
        // monitor the result, since the bailout happens before the MMonitorTypes
        // instruction is executed.
        jsbytecode *pc = activation->bailout()->bailoutPc();

        // If this is not a ResumeAfter bailout, there's nothing to monitor,
        // we will redo the op in the interpreter.
        bool isResumeAfter = GetNextPc(pc) == cx->regs().pc;

        if ((js_CodeSpec[*pc].format & JOF_TYPESET) && isResumeAfter) {
            JS_ASSERT(retval == BAILOUT_RETURN_OK);
            return BAILOUT_RETURN_MONITOR;
        }

        return retval;
    }

    return BAILOUT_RETURN_FATAL_ERROR;
}

static void
ReflowArgTypes(JSContext *cx)
{
    StackFrame *fp = cx->fp();
    unsigned nargs = fp->fun()->nargs;
    RootedScript script(cx, fp->script());

    types::AutoEnterTypeInference enter(cx);

    if (!fp->isConstructing())
        types::TypeScript::SetThis(cx, script, fp->thisValue());
    for (unsigned i = 0; i < nargs; ++i)
        types::TypeScript::SetArgument(cx, script, i, fp->unaliasedFormal(i, DONT_CHECK_ALIASING));
}

uint32_t
ion::ReflowTypeInfo(uint32_t bailoutResult)
{
    JSContext *cx = GetIonContext()->cx;
    IonActivation *activation = cx->runtime->ionActivation;

    IonSpew(IonSpew_Bailouts, "reflowing type info");

    if (bailoutResult == BAILOUT_RETURN_ARGUMENT_CHECK) {
        IonSpew(IonSpew_Bailouts, "reflowing type info at argument-checked entry");
        ReflowArgTypes(cx);
        return true;
    }

    RootedScript script(cx, cx->fp()->script());
    jsbytecode *pc = activation->bailout()->bailoutPc();

    JS_ASSERT(js_CodeSpec[*pc].format & JOF_TYPESET);

    IonSpew(IonSpew_Bailouts, "reflowing type info at %s:%d pcoff %d", script->filename,
            script->lineno, pc - script->code);

    types::AutoEnterTypeInference enter(cx);
    if (bailoutResult == BAILOUT_RETURN_TYPE_BARRIER)
        script->analysis()->breakTypeBarriers(cx, pc - script->code, false);
    else
        JS_ASSERT(bailoutResult == BAILOUT_RETURN_MONITOR);

    // When a type barrier fails, the bad value is at the top of the stack.
    Value &result = cx->regs().sp[-1];
    types::TypeScript::Monitor(cx, script, pc, result);

    return true;
}

uint32_t
ion::RecompileForInlining()
{
    JSContext *cx = GetIonContext()->cx;
    RawScript script = cx->fp()->script().unsafeGet();

    IonSpew(IonSpew_Inlining, "Recompiling script to inline calls %s:%d", script->filename,
            script->lineno);

    // Invalidate the script to force a recompile.
    if (!Invalidate(cx, script, /* resetUses */ false))
        return BAILOUT_RETURN_FATAL_ERROR;

    // Invalidation should not reset the use count.
    JS_ASSERT(script->getUseCount() >= js_IonOptions.usesBeforeInlining);

    return true;
}

bool
ion::EnsureHasCallObject(JSContext *cx, StackFrame *fp)
{
    if (fp->isFunctionFrame() &&
        fp->fun()->isHeavyweight() &&
        !fp->hasCallObj())
    {
        return fp->initCallObject(cx);
    }
    return true;
}

uint32_t
ion::BoundsCheckFailure()
{
    JSContext *cx = GetIonContext()->cx;
    JSScript *script = GetBailedJSScript(cx);

    IonSpew(IonSpew_Bailouts, "Bounds check failure %s:%d", script->filename,
            script->lineno);

    if (!script->failedBoundsCheck) {
        script->failedBoundsCheck = true;

        // Invalidate the script to force a recompile.
        IonSpew(IonSpew_Invalidate, "Invalidating due to bounds check failure");

        return Invalidate(cx, script);
    }

    return true;
}

uint32_t
ion::ShapeGuardFailure()
{
    JSContext *cx = GetIonContext()->cx;
    JSScript *script = GetBailedJSScript(cx);

    JS_ASSERT(script->hasIonScript());
    JS_ASSERT(!script->ion->invalidated());

    script->failedShapeGuard = true;

    IonSpew(IonSpew_Invalidate, "Invalidating due to shape guard failure");

    return Invalidate(cx, script);
}

uint32_t
ion::CachedShapeGuardFailure()
{
    JSContext *cx = GetIonContext()->cx;
    JSScript *script = GetBailedJSScript(cx);

    JS_ASSERT(script->hasIonScript());
    JS_ASSERT(!script->ion->invalidated());

    script->failedShapeGuard = true;

    // Purge JM caches in the script and all inlined script, to avoid baking in
    // the same shape guard next time.
    for (size_t i = 0; i < script->ion->scriptEntries(); i++)
        mjit::PurgeCaches(script->ion->getScript(i));

    IonSpew(IonSpew_Invalidate, "Invalidating due to shape guard failure");

    return Invalidate(cx, script);
}

uint32_t
ion::ThunkToInterpreter(Value *vp)
{
    JSContext *cx = GetIonContext()->cx;
    IonActivation *activation = cx->runtime->ionActivation;
    BailoutClosure *br = activation->takeBailout();

    if (!EnsureHasCallObject(cx, cx->fp()))
        return Interpret_Error;

    // By default we set the forbidOsr flag on the ion script, but if a GC
    // happens just after we re-enter the interpreter, the ion script get
    // invalidated and we do not see the forbidOsr flag.  This may cause a loop
    // which apear with eager compilation and gc zeal enabled.  This code is a
    // workaround to avoid recompiling with OSR just after a bailout followed by
    // a GC. (see Bug 746691 & Bug 751383)
    jsbytecode *pc = cx->regs().pc;
    while (JSOp(*pc) == JSOP_GOTO)
        pc += GET_JUMP_OFFSET(pc);
    if (JSOp(*pc) == JSOP_LOOPENTRY)
        cx->regs().pc = GetNextPc(pc);

    // When JSScript::argumentsOptimizationFailed, we cannot access Ion frames
    // in order to create an arguments object for them.  However, there is an
    // invariant that script->needsArgsObj() implies fp->hasArgsObj() (after the
    // prologue), so we must create one now for each inlined frame which needs
    // one.
    {
        br->entryfp()->clearRunningInIon();
        ScriptFrameIter iter(cx);
        StackFrame *fp = NULL;
        Rooted<JSScript*> script(cx, NULL);
        do {
            fp = iter.interpFrame();
            script = iter.script();
            if (script->needsArgsObj()) {
                // Currently IonMonkey does not compile if the script needs an
                // arguments object, so the frame should not have any argument
                // object yet.
                JS_ASSERT(!fp->hasArgsObj());
                ArgumentsObject *argsobj = ArgumentsObject::createExpected(cx, fp);
                if (!argsobj)
                    return Interpret_Error;
                InternalBindingsHandle bindings(script, &script->bindings);
                const unsigned var = Bindings::argumentsVarIndex(cx, bindings);
                // The arguments is a local binding and needsArgsObj does not
                // check if it is clobbered. Ensure that the local binding
                // restored during bailout before storing the arguments object
                // to the slot.
                if (fp->unaliasedLocal(var).isMagic(JS_OPTIMIZED_ARGUMENTS))
                    fp->unaliasedLocal(var) = ObjectValue(*argsobj);
            }
            ++iter;
        } while (fp != br->entryfp());
    }

    if (activation->entryfp() == br->entryfp()) {
        // If the bailout entry fp is the same as the activation entryfp, then
        // there are no scripted frames below us. In this case, just shortcut
        // out with a special return code, and resume interpreting in the
        // original Interpret activation.
        vp->setMagic(JS_ION_BAILOUT);
        js_delete(br);
        return Interpret_Ok;
    }

    InterpretStatus status = Interpret(cx, br->entryfp(), JSINTERP_BAILOUT);

    if (status == Interpret_OSR) {
        // The interpreter currently does not ask to perform inline OSR, so
        // this path is unreachable.
        JS_NOT_REACHED("invalid");

        IonSpew(IonSpew_Bailouts, "Performing inline OSR %s:%d",
                cx->fp()->script()->filename,
                PCToLineNumber(cx->fp()->script().unsafeGet(), cx->regs().pc));

        // We want to OSR again. We need to avoid the problem where frequent
        // bailouts cause recursive nestings of Interpret and EnterIon. The
        // interpreter therefore shortcuts out, and now we're responsible for
        // completing the OSR inline.
        //
        // Note that we set runningInIon so that if we re-enter C++ from within
        // the inlined OSR, StackIter will know to traverse these frames.
        StackFrame *fp = cx->fp();

        fp->setRunningInIon();
        vp->setPrivate(fp);
        js_delete(br);
        return Interpret_OSR;
    }

    if (status == Interpret_Ok)
        *vp = br->entryfp()->returnValue();

    // The BailoutFrameGuard's destructor will ensure that the frame is
    // removed.
    js_delete(br);

    return status;
}

