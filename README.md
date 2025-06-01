# cambda
Typed Lambda-Calculus interpreter in C

```scheme

(type ListNode of t
    (value t)
    (next Maybe ListNode of t))

(type BinTree of t
    (Leaf t)
    (Split (struct
        (Left BinTree t)
        (Right BinTree t))))

(define sum: Int->Int->Int
    (fn (a b) (+ a b)))

``` 

Locals stored on the stack, can be accessed with frame pointer offsets
mapping (var name => frame pointer offsets)

First, a cbValue is pushed to make space for the return value
Then, the frame pointer is pointed to the next free slot on the array
Then, the arguments are pushed
To access the return value: fp - 1
To access the args: fp + n(-1)
