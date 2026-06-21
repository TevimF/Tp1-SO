// user/test_tp3.c
// Suíte de testes para o TP3 — Proteção contra Null Pointer Dereference
// Cobre: página 0 inválida, page faults, validação de ponteiros,
//        herança após fork, ausência de vazamentos e realocação.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// ─── utilitários ──────────────────────────────────────────────────────────

static void
pass(char *msg)
{
  printf("  \033[32mOK\033[0m  : %s\n", msg);
}

static void
fail(char *msg)
{
  printf("  \033[31mFAIL\033[0m: %s\n", msg);
}

// Executa fn em um processo filho.
// Retorna 1 se o filho foi morto pelo kernel (exit status == -1).
static int
expect_killed(void (*fn)(void))
{
  int pid = fork();
  if(pid == 0){
    fn();
    // se chegou aqui o processo NÃO foi morto — falha
    exit(0);
  }
  int status;
  wait(&status);
  return status == -1;
}

// ─── corpos dos filhos (testes que devem matar o processo) ────────────────

static void do_write_null(void)   { volatile int  *p = (int*)0;    *p = 42;   }
static void do_read_null(void)    { volatile int  *p = (int*)0;    (void)*p;  }
static void do_access_8(void)     { volatile int  *p = (int*)8;    *p = 1;    }
static void do_access_4095(void)  { volatile char *p = (char*)4095; *p = 1;   }
static void do_null_child(void)   { volatile int  *p = (int*)0;    *p = 99;   }

// ─── âncora para o teste de realocação ───────────────────────────────────

static void anchor(void) { /* endereço desta função deve ser >= 0x1000 */ }

// ══════════════════════════════════════════════════════════════════════════
// Teste 1 — Escrita em NULL
// Verifica que uma store em 0x0 gera page fault e mata o processo.
// ══════════════════════════════════════════════════════════════════════════
static void
test1(void)
{
  printf("Teste 1 - Escrita em NULL (0x0):\n");
  if(expect_killed(do_write_null))
    pass("store em 0x0 gera page fault e mata o processo");
  else
    fail("store em 0x0 deveria matar o processo");
}

// ══════════════════════════════════════════════════════════════════════════
// Teste 2 — Leitura em NULL
// Verifica que um load de 0x0 gera page fault e mata o processo.
// ══════════════════════════════════════════════════════════════════════════
static void
test2(void)
{
  printf("Teste 2 - Leitura em NULL (0x0):\n");
  if(expect_killed(do_read_null))
    pass("load de 0x0 gera page fault e mata o processo");
  else
    fail("load de 0x0 deveria matar o processo");
}

// ══════════════════════════════════════════════════════════════════════════
// Teste 3 — Acesso ao endereço 8
// Qualquer endereço dentro da página 0 (0x0–0xFFF) deve ser inválido.
// ══════════════════════════════════════════════════════════════════════════
static void
test3(void)
{
  printf("Teste 3 - Acesso ao endereço 8 (0x8, ainda na página 0):\n");
  if(expect_killed(do_access_8))
    pass("acesso a 0x8 (página 0) mata o processo");
  else
    fail("acesso a 0x8 deveria matar o processo — página 0 deve ser inválida");
}

// ══════════════════════════════════════════════════════════════════════════
// Teste 4 — Acesso ao endereço 4095
// 0xFFF é o último byte da página 0. Deve também ser inválido.
// ══════════════════════════════════════════════════════════════════════════
static void
test4(void)
{
  printf("Teste 4 - Acesso ao endereço 4095 (0xFFF, último byte da página 0):\n");
  if(expect_killed(do_access_4095))
    pass("acesso a 0xFFF (último byte da página 0) mata o processo");
  else
    fail("acesso a 0xFFF deveria matar o processo");
}

// ══════════════════════════════════════════════════════════════════════════
// Teste 5 — write() com ponteiro NULL
// O kernel deve rejeitar a syscall com -1, NÃO matar o processo.
// copyin(pagetable, dst, 0, n) falha porque página 0 não está mapeada.
// ══════════════════════════════════════════════════════════════════════════
static void
test5(void)
{
  printf("Teste 5 - write() com buffer NULL:\n");
  int r = write(1, (void*)0, 4);
  if(r <= 0)
    pass("write(fd, NULL, 4) retorna erro sem matar o processo");
  else
    fail("write(fd, NULL, 4) deveria retornar erro (retornou != -1)");
}

// ══════════════════════════════════════════════════════════════════════════
// Teste 6 — open() com pathname NULL
// fetchstr/copyinstr falha ao ler o caminho do endereço 0.
// A syscall deve retornar -1, sem matar o processo.
// ══════════════════════════════════════════════════════════════════════════
static void
test6(void)
{
  printf("Teste 6 - open() com pathname NULL:\n");
  int fd = open((char*)0, O_RDONLY);
  if(fd < 0)
    pass("open(NULL, O_RDONLY) retorna erro sem matar o processo");
  else{
    fail("open(NULL, O_RDONLY) deveria retornar erro");
    close(fd);
  }
}

// ══════════════════════════════════════════════════════════════════════════
// Teste 7 — fork() + NULL no filho
// Verifica que o filho herda a proteção: acesso NULL no filho mata só
// o filho; o pai continua executando normalmente.
// ══════════════════════════════════════════════════════════════════════════
static void
test7(void)
{
  printf("Teste 7 - fork() + acesso NULL no filho:\n");
  if(expect_killed(do_null_child))
    pass("filho morre ao acessar NULL; pai sobrevive e continua");
  else
    fail("filho deveria ter morrido — proteção não foi herdada via fork");
}

// ══════════════════════════════════════════════════════════════════════════
// Teste 8 — Stress: sem vazamento de memória
// Realiza 500 ciclos de sbrk(+PGSIZE)/sbrk(-PGSIZE) e verifica que o
// brk retorna ao valor inicial (nenhum pagina vazou).
// Também confirma que o heap começa em >= PGSIZE (exec.c: sz = PGSIZE).
// ══════════════════════════════════════════════════════════════════════════
static void
test8(void)
{
  printf("Teste 8 - Stress: verificação de vazamento de memória:\n");

  // 8a: heap não começa em 0 (sz = PGSIZE em exec.c)
  char *base = sbrk(0);
  if((uint64)base >= 4096)
    pass("heap começa em >= 0x1000 (exec.c: sz = PGSIZE correto)");
  else
    fail("heap começa abaixo de 0x1000 — exec.c não foi corrigido");

  // 8b: 500 ciclos alloc/free sem crescimento
  int pgsize = 4096;
  int rounds = 500;
  char *start = sbrk(0);
  int ok = 1;

  for(int i = 0; i < rounds; i++){
    char *p = sbrk(pgsize);
    if(p == (char*)-1){ ok = 0; break; }
    p[0] = (char)i;         // toca a página (garante alocação real)
    p[pgsize - 1] = (char)i;
    if(sbrk(-pgsize) == (char*)-1){ ok = 0; break; }
  }

  char *end = sbrk(0);
  if(!ok)
    fail("sbrk() falhou durante o stress — sistema sem memória?");
  else if(end != start)
    fail("heap cresceu após 500 ciclos alloc/free (possível vazamento)");
  else
    pass("500 ciclos alloc/free: heap retorna ao tamanho original");
}

// ══════════════════════════════════════════════════════════════════════════
// Teste 9 — Realocação dos executáveis
// Com user.ld ajustado para 0x1000, todo código de usuário deve estar
// em endereços >= 0x1000. Testa via endereço da função anchor().
// ══════════════════════════════════════════════════════════════════════════
static void
test9(void)
{
  printf("Teste 9 - Realocação: segmento de texto >= 0x1000:\n");
  uint64 addr = (uint64)anchor;
  if(addr >= 0x1000){
    pass("segmento de texto começa em >= 0x1000 (user.ld correto)");
    printf("         anchor() está em 0x%lx\n", addr);
  } else {
    fail("segmento de texto em < 0x1000 — user.ld não foi ajustado?");
    printf("         anchor() está em 0x%lx (esperado >= 0x1000)\n", addr);
  }
}

// ══════════════════════════════════════════════════════════════════════════
int
main(void)
{
  printf("\n=== TP3: Suíte de Testes — Proteção contra Null Pointer ===\n\n");

  test1();
  test2();
  test3();
  test4();
  test5();
  test6();
  test7();
  test8();
  test9();

  printf("\n=== Fim da suíte TP3 ===\n");
  exit(0);
}