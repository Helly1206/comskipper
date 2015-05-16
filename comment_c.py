#!/usr/bin/python

import sys
from tempfile import mkstemp
from shutil import move
from os import remove, close
from random import randrange

OUTFILE = "temp.mod"

def RemoveComment(line, inBlock):
    inBlockOut=inBlock
    lineOut = ""
    ProcessComment = True
    if CompareLine(line,"//"):
        lineProc=line.split("//",1)[0]
    else:
        lineProc=line
    while ProcessComment:
        if (inBlockOut):
            if CompareLine(lineProc,"*/"):
                inBlockOut=False
                lineProc=lineProc.split("*/",1)[1]
                if not CompareLine(lineProc,"/*"):
                    ProcessComment = False
                    lineOut=lineOut+lineProc
                else:
                    lineOut=lineOut+lineProc.split("/*",1)[0]
                    lineProc="/*"+lineProc.split("/*",1)[1]
            else:
                ProcessComment = False
        else:
            if CompareLine(lineProc,"/*"):
                inBlockOut=True
                lineOut=lineOut+lineProc.split("/*",1)[0]
                lineProc=lineProc.split("/*",1)[1]
                if not CompareLine(lineProc,"*/"):
                    ProcessComment = False
            else:
                lineOut=lineProc
                ProcessComment = False
    return lineOut,inBlockOut

def CountAcc(line, inBlock, accCtr):
    ProcessAcc = True
    accCtrOut = accCtr
    tempLine,inBlockOut = RemoveComment(line, inBlock)
    while ProcessAcc:
        if CompareLine(tempLine,"{"):
            tempLine=tempLine.split("{",1)[1]
            accCtrOut+=1
        if CompareLine(tempLine,"}"):
            tempLine=tempLine.split("}",1)[1]
            accCtrOut-=1
        if not CompareLine(tempLine,"{") and not CompareLine(tempLine,"}"):
            ProcessAcc = False
    

    return inBlockOut, accCtrOut

def CompareLine(line, exline):
    rv = False
    if exline in line:
        rv = True
    return rv

def CommentOut(file_path, Expression, Macro):
    AccCtr = 0
    InBlockCmt = False
    ExpressionFound = False
    ExLine = 0
    tmpLines = []
    #Create temp file
    fh, abs_path = mkstemp()
    with open(abs_path,'w') as new_file:
        with open(file_path) as old_file:
            for line in old_file:
                if CompareLine(line,Expression[ExLine]):
                    ExLine+=1
                    if ExLine >= len(Expression):
                        ExpressionFound = True
                        AccCtr = 0
                        new_file.write("#ifdef %s\n"%Macro)
                        for ln in tmpLines:
                            InBlockCmt,AccCtr = CountAcc(ln,InBlockCmt,AccCtr)
                            new_file.write(ln)
                        del tmpLines
                        tmpLines = []
                        ExLine = 0
                    else:
                        tmpLines.append(line)
                else:
                    if ExLine > 0:
                        # False expression
                        for ln in tmpLines:
                            new_file.write(ln)
                        del tmpLines
                        tmpLines = []
                        ExpressionFound = False
                        ExLine = 0
                        AccCtr = 0
                if ExLine == 0:
                    new_file.write(line)
                    if ExpressionFound:
                        InBlockCmt,AccCtr = CountAcc(line,InBlockCmt,AccCtr)
                        if AccCtr <= 0:
                            ExpressionFound = False
                            new_file.write("#endif\n")
    close(fh)
    #Remove original file
    remove(file_path)
    #Move new file
    move(abs_path, file_path)    

#########################################################
# Main                                                  #
#########################################################
### 

if len(sys.argv) > 2:
    FileName = sys.argv[1]
    ExprStr = sys.argv[2]
    Expr = ExprStr.split('\\n')
    if len(sys.argv) > 3:
        MacroStr = sys.argv[3]
    else:
        irand = randrange(0, 10000)
        MacroStr="CO_%d"%irand
else:
    print sys.argv[0]
    print "Usage: %s: <FilePath> <Expression> <Macro>"%sys.argv[0]
    print "    FilePath = complete path to file"
    print "    Expression = Epression as start of function (multiline = \\n)"
    print "    Macro = Optional #ifdef marco to comment out (default: random)"
    exit()

print "Start file modification '%s'" % (FileName)

CommentOut(FileName,Expr,MacroStr)
#["namespace CS {","","void FramesPerSecond::print(bool final)"]

print "Finished"



