// ============================================================================
// Highlight.js grammar registration for The Pith Programming Language
// ============================================================================

window.addEventListener('DOMContentLoaded', () => {
    if (typeof hljs === 'undefined') return;

    hljs.registerLanguage('pith', (hljs) => {
        return {
            name: 'Pith',
            aliases: ['pi'],
            keywords: {
                keyword: 'fn end if elseif else while break continue mut return import print and or not',
                type: 'i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 bool string',
                literal: 'true false',
                built_in: 'os net root identifyKernel identifyKernelVersion isNT isLinux isFreeBSD isDarwin isMacOS readFile writeFile getEnv exit argCount getArg socket connect send recv close'
            },
            contains: [
                hljs.HASH_COMMENT_MODE,
                hljs.QUOTE_STRING_MODE,
                hljs.C_NUMBER_MODE,
                {
                    className: 'title.function',
                    begin: '\\bfn\\s+([a-zA-Z_][a-zA-Z0-9_]*)',
                    returnBegin: true,
                    contains: [
                        {
                            begin: '\\b(fn)\\b',
                            className: 'keyword'
                        },
                        {
                            begin: '[a-zA-Z_][a-zA-Z0-9_]*',
                            className: 'title.function'
                        }
                    ]
                }
            ]
        };
    });

    // Trigger re-highlight on all Pith blocks
    document.querySelectorAll('code.language-pith, code.language-pi').forEach((block) => {
        hljs.highlightElement(block);
    });
});
