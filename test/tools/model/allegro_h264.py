# -*- coding: utf-8 -*-

'''
================================================================================

  This confidential and proprietary software may be used only
 as authorized by a licensing agreement from Thumb o'Cat Inc.
 In the event of publication, the following notice is applicable:

      Copyright (C) 2013 - 2014 Thumb o'Cat
                    All right reserved.

  The entire notice above must be reproduced on all authorized copies.

================================================================================

 File      : allegro_264.py
 Author(s) : Luuvish
 Version   : 2.0
 Revision  :
     1.0 May 19, 2013    first release
     2.0 May 12, 2014    Executor classify

================================================================================
'''

__all__ = ('AllegroH264', )

__version__ = '2.0.0'

from .jm_18_6 import JM


class AllegroH264(JM):

    model   = 'allegro-h264'
    codecs  = ('h264', )
    actions = ('decode', 'digest_by_frames', 'compare')

    def __init__(self, codec='h264', **kwargs):
        from os.path import join, normpath, dirname

        super(AllegroH264, self).__init__(codec, **kwargs)

        self._execute = normpath(join(dirname(__file__), 'ldecod_Allegro.exe'))

        self._config = '''\
%s                       ........H.264/AVC coded bitstream
%s                       ........Output file, YUV/RGB
%s                       ........Ref sequence (for SNR)
1                        ........Write 4:2:0 chroma components for monochrome streams
0                        ........NAL mode (0=Annex B, 1: RTP packets)
0                        ........SNR computation offset
2                        ........Poc Scale (1 or 2)
500000                   ........Rate_Decoder
104000                   ........B_decoder
73000                    ........F_decoder
leakybucketparam.cfg     ........LeakyBucket Params
0                        ........Err Concealment(0:Off,1:Frame Copy,2:Motion Copy)
2                        ........Reference POC gap (2: IPP (Default), 4: IbP / IpP)
2                        ........POC gap (2: IPP /IbP/IpP (Default), 4: IPP with frame skip = 1 etc.)
0                        ........Silent decode

This is a file containing input parameters to the JVT H.264/AVC decoder.
The text line following each parameter is discarded by the decoder.
'''

    def options(self, source, target):
        return [source]
